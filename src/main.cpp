// glassbox-dvp-sim: the command-line front end.
//
//   glassbox-dvp-sim                        how-to-play menu (in a terminal)
//   glassbox-dvp-sim --menu                 the menu, even without a terminal
//   glassbox-dvp-sim --help                 every command and flag
//   glassbox-dvp-sim --list
//   glassbox-dvp-sim --scenario S1 [--seed 1]            full trace at once
//   glassbox-dvp-sim --scenario S1 --watch [options]     glass-box player
//   glassbox-dvp-sim --scenario S1 --story               narrated, plain text
//   glassbox-dvp-sim --tour [options]                    S1, S2, S6, S9, S14
//
// Player options: --speed slow|normal|fast|<25..800>, --paused, --no-color.
// Settings (with --watch, --story or a plain run): --set key=value, e.g.
//   --set vote_timeout=30ms  --set latency.COORD-BANK_B=80ms
//   --set drop.COORD-BANK_A=20%  --set restart_after=2s
// Exit codes: 0 quiesced, 2 usage error, 3 event bound hit (not quiesced).
//
// The player records the run first (cli/recording) and then only paces
// what it shows; the simulation itself never waits or slows down.

#include "cli/dials.hpp"
#include "cli/guide.hpp"
#include "cli/narrator.hpp"
#include "cli/pacing.hpp"
#include "cli/recording.hpp"
#include "cli/screen.hpp"
#include "cli/terminal.hpp"
#include "scenarios/scenario.hpp"
#include "trace/trace_format.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace cli = glassbox::cli;

// Recordings stop here if a run never quiesces (e.g. a node never returns).
constexpr std::size_t k_max_player_events = 20'000;
constexpr std::int64_t k_tick_ms = 40;
// Intro and end cards move on by themselves after this long (at 100%).
constexpr std::int64_t k_card_ms = 12'000;

class StdoutSink : public glassbox::TraceSink {
  public:
    void write(const glassbox::TraceRecord& record) override
    {
        std::cout << glassbox::format_trace_line(record) << '\n';
    }
};

// Prints a short usage reminder and returns the usage exit code.
int usage()
{
    std::cerr
        << "usage: glassbox-dvp-sim            (menu; or --help for everything)\n"
           "       glassbox-dvp-sim --list\n"
           "       glassbox-dvp-sim --scenario <ID> [--seed <N>]\n"
           "       glassbox-dvp-sim --scenario <ID> --watch [--speed "
           "slow|normal|fast|<25..800>] [--paused] [--no-color]\n"
           "       glassbox-dvp-sim --scenario <ID> --story\n"
           "       glassbox-dvp-sim --tour [--speed ...] [--no-color]\n"
           "settings (repeatable): --set vote_timeout=30ms | "
           "latency.COORD-BANK_B=80ms | drop.COORD-BANK_B=20% | "
           "restart_after=2s\n";
    return 2;
}

[[nodiscard]] std::optional<std::uint64_t> parse_seed(std::string_view text)
{
    std::uint64_t value = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<int> parse_speed(std::string_view text)
{
    if (text == "slow") {
        return 50;
    }
    if (text == "normal") {
        return 100;
    }
    if (text == "fast") {
        return 200;
    }
    int value = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() ||
        value < cli::k_speed_percent.front() ||
        value > cli::k_speed_percent.back()) {
        return std::nullopt;
    }
    return value;
}

void print_final_state(const glassbox::ScenarioRun& run)
{
    const glassbox::ProtocolNodes& nodes = run.nodes();
    for (const auto& [id, state] : nodes.coordinator->trades()) {
        std::cout << "final COORD trade=" << id.value() << " "
                  << glassbox::to_string(state.state) << '\n';
    }
    for (const glassbox::Participant* bank : nodes.banks()) {
        const glassbox::LocalLedger& ledger = bank->ledger();
        std::cout << "final " << glassbox::to_string(bank->self())
                  << " cash_total=" << ledger.cash_total().value()
                  << " cash_locked=" << ledger.cash_locked().value()
                  << " bonds_total=" << ledger.bonds_total().value()
                  << " bonds_locked=" << ledger.bonds_locked().value();
        for (const auto& [id, trade] : bank->trades()) {
            std::cout << " trade" << id.value() << "="
                      << glassbox::to_string(trade.state);
        }
        std::cout << '\n';
    }
}

// --- --story: the narration as plain text -----------------------------------

void print_story(const cli::Recording& rec)
{
    for (std::size_t i = 1; i < rec.frames.size(); ++i) {
        const cli::Story s = cli::narrate(rec, i);
        const std::string time = cli::format_time(rec.frames[i].time_us);
        std::string stamp = "[" + std::string(14 - std::min<std::size_t>(
                                                       14, time.size()),
                                              ' ') +
                            time + "] ";
        std::cout << stamp << s.headline << '\n';
        if (cli::is_minor(rec.frames[i])) {
            continue;
        }
        const std::string pad(stamp.size(), ' ');
        for (const std::string& d : s.details) {
            std::cout << pad << "· " << d << '\n';
        }
        if (!s.why.empty()) {
            std::cout << pad << "why  " << s.why << '\n';
        }
        if (!s.code.empty()) {
            std::cout << pad << "code " << s.code << '\n';
        }
    }
}

// --- --watch: the paced, full-screen player ---------------------------------

enum class Flow { go, skip, restart, quit };

// What the viewer chose at the end of a scenario.
enum class Next { next, quit };

struct Player {
    cli::Terminal& term;
    const cli::Recording& rec;
    const cli::ScenarioGuide* guide;
    std::string title;
    int speed;
    bool paused;
    bool color;

    [[nodiscard]] int width() const
    {
        return std::clamp(term.columns(), 80, 200);
    }

    [[nodiscard]] std::string banner() const
    {
        const std::string head = "GLASS-BOX DvP SETTLEMENT · " + title;
        const std::string note = "simplified educational model";
        const std::size_t w = static_cast<std::size_t>(width());
        const std::size_t gap = w > cli::visible_width(head) + note.size()
                                    ? w - cli::visible_width(head) - note.size()
                                    : 1;
        return (color ? "\x1b[1;7m" : "") + head + std::string(gap, ' ') +
               note + (color ? "\x1b[0m" : "");
    }

    [[nodiscard]] std::string footer(std::string_view keys) const
    {
        return std::string{color ? "\x1b[2m" : ""} + std::string{keys} +
               (color ? "\x1b[0m" : "");
    }

    // A card (intro or end) held until a key or k_card_ms.
    [[nodiscard]] cli::Key show_card(std::vector<std::string> body,
                                     std::string_view keys)
    {
        std::vector<std::string> lines{banner(), ""};
        for (std::string& line : body) {
            lines.push_back(std::move(line));
        }
        lines.emplace_back();
        lines.push_back(footer(keys));
        term.show(lines);
        for (std::int64_t waited = 0; waited < k_card_ms * 100 / speed;
             waited += 250) {
            const cli::Key key = term.wait_key(250);
            if (key == cli::Key::faster) {
                speed = cli::faster(speed);
            } else if (key == cli::Key::slower) {
                speed = cli::slower(speed);
            } else if (key != cli::Key::none) {
                return key;
            }
        }
        return cli::Key::none;
    }

    void render(std::size_t frame, glassbox::SimTimeUs now)
    {
        const int width = this->width();
        std::vector<std::string> lines;
        lines.push_back(banner());
        for (std::string& line :
             cli::draw(rec, cli::Moment{frame, now, speed, paused},
                       cli::ScreenOptions{width, color})) {
            lines.push_back(std::move(line));
        }
        lines.emplace_back();
        lines.push_back(footer("[space] pause   [n] next event   [+/-] speed   "
                               "[r] restart   [q] quit"));
        term.show(lines);
    }

    // Waits ms of playing time (forever while paused), handling keys.
    Flow wait(std::int64_t ms, const std::function<void()>& redraw)
    {
        std::int64_t left = ms;
        while (paused || left > 0) {
            const std::int64_t slice = paused ? 250 : std::min(left, k_tick_ms);
            switch (term.wait_key(slice)) {
            case cli::Key::pause:
                paused = !paused;
                redraw();
                break;
            case cli::Key::next:
                return Flow::skip;
            case cli::Key::faster:
                speed = cli::faster(speed);
                redraw();
                break;
            case cli::Key::slower:
                speed = cli::slower(speed);
                redraw();
                break;
            case cli::Key::restart:
                return Flow::restart;
            case cli::Key::quit:
                return Flow::quit;
            case cli::Key::none:
                if (!paused) {
                    left -= slice;
                }
                break;
            }
        }
        return Flow::go;
    }

    // Intro card, the run, then the end. Returns what the viewer chose.
    Next play()
    {
        if (guide) {
            const cli::Key key = show_card(
                cli::draw_intro(rec, *guide, cli::ScreenOptions{width(), color}),
                "[n] or Enter: start   [+/-] speed   [q] quit   "
                "(or wait: it starts by itself)");
            if (key == cli::Key::quit) {
                return Next::quit;
            }
        }
        std::size_t frame = 1;
        bool skip_travel = false;
        render(0, 0);
        if (wait(cli::k_dwell_ms * 100 / speed, [&] { render(0, 0); }) ==
            Flow::quit) {
            return Next::quit;
        }
        while (frame < rec.frames.size()) {
            const glassbox::SimTimeUs t0 = rec.frames[frame - 1].time_us;
            const glassbox::SimTimeUs t1 = rec.frames[frame].time_us;
            const bool flying = !cli::in_flight(rec, t0).empty();
            glassbox::SimTimeUs now = t0;
            Flow flow = Flow::go;
            // Animate the gap: messages move, timers count down.
            while (!skip_travel && now < t1) {
                const cli::Pace p = cli::pace(t1 - t0, flying, false, speed);
                if (p.travel_ms <= 0) {
                    break;
                }
                render(frame - 1, now);
                flow = wait(k_tick_ms, [&] { render(frame - 1, now); });
                if (flow != Flow::go) {
                    break;
                }
                const auto step = glassbox::checked_multiply_i64(t1 - t0, k_tick_ms);
                const glassbox::SimTimeUs advance =
                    step ? std::max<glassbox::SimTimeUs>(*step / p.travel_ms, 1)
                         : t1 - now;
                now = advance >= t1 - now ? t1 : now + advance;
            }
            skip_travel = false;
            if (flow == Flow::quit) {
                return Next::quit;
            }
            if (flow == Flow::restart) {
                frame = 1;
                render(0, 0);
                continue;
            }
            // Show the event and hold it.
            render(frame, t1);
            const cli::Pace hold =
                cli::pace(0, false, cli::is_minor(rec.frames[frame]), speed);
            flow = wait(hold.dwell_ms, [&] { render(frame, t1); });
            if (flow == Flow::quit) {
                return Next::quit;
            }
            if (flow == Flow::restart) {
                frame = 1;
                render(0, 0);
                continue;
            }
            // Stepping while paused jumps straight to the next event.
            skip_travel = flow == Flow::skip && paused;
            ++frame;
        }
        // End card until a key (or k_card_ms).
        if (guide) {
            const cli::Key key = show_card(
                cli::draw_summary(rec, *guide, cli::ScreenOptions{width(), color}),
                "[n] or Enter: continue   [r] replay   [q] quit");
            if (key == cli::Key::restart) {
                return play();
            }
            return key == cli::Key::quit ? Next::quit : Next::next;
        }
        return Next::next;
    }
};

[[nodiscard]] std::string title_of(const glassbox::ScenarioInfo& info,
                                   std::uint64_t seed)
{
    return std::string{info.id} + " " + std::string{info.name} + " · seed " +
           std::to_string(seed);
}

// Settings shared by the command line and the menu.
struct Options {
    std::uint64_t seed = 1;
    int speed = cli::k_default_speed_percent;
    bool paused = false;
    bool color = true;
    std::vector<std::string> settings; // "key=value", see cli/dials.hpp
};

// Returns "slow (50%)", "normal (100%)", "fast (200%)" or "N%".
[[nodiscard]] std::string speed_name(int speed)
{
    switch (speed) {
    case 50:
        return "slow (50%)";
    case 100:
        return "normal (100%)";
    case 200:
        return "fast (200%)";
    default:
        return std::to_string(speed) + "%";
    }
}

constexpr std::string_view k_settings_help =
    "  Settings change the simulated system before a scenario runs; they\n"
    "  apply to every scenario until you clear them.\n"
    "\n"
    "    vote_timeout=30ms          how long the coordinator waits for both\n"
    "                               votes before it aborts (default 500ms)\n"
    "    latency.COORD-BANK_B=80ms  one-way delay of a link, both directions\n"
    "    drop.COORD-BANK_B=20%      share of messages a link loses (0-100%)\n"
    "    restart_after=2s           how long a crashed node stays down\n"
    "                               (scenarios with a crash: 6, 7, 8, 9, 10)\n"
    "\n"
    "  Nodes: COORD, BANK_A, BANK_B, BANK_C. Units: us, ms, s; % for drop.\n"
    "  Try: set vote_timeout=30ms, then 1 (the happy path now aborts), or\n"
    "  set vote_timeout=900ms, then 4 (the false abort now commits).\n";

constexpr std::string_view k_keys_help =
    "  While a scenario plays:\n"
    "    space      pause / resume\n"
    "    n, Enter   next event (while paused: one step per press)\n"
    "    + / -      faster / slower (25% to 800%)\n"
    "    r          restart this scenario\n"
    "    q          stop and go back (to the menu, or to the shell)\n";

// The complete command-line reference, printed by --help.
void print_manual(std::ostream& out)
{
    out << "glassbox-dvp-sim: a glass-box simulator of atomic delivery-versus-"
           "payment (DvP)\nsettlement between banks, using two-phase commit. "
           "A simplified educational model.\n"
           "\n"
           "START HERE\n"
           "  make                  open the how-to-play menu (pick, watch, "
           "come back, repeat)\n"
           "  make tour             guided demo: scenarios 1, 2, 6, 9, 14\n"
           "\n"
           "COMMANDS\n"
           "  glassbox-dvp-sim                        the menu (in a terminal; "
           "otherwise this manual)\n"
           "  glassbox-dvp-sim --menu                 the menu, even without a "
           "terminal\n"
           "  glassbox-dvp-sim --tour                 S1, S2, S6, S9, S14 one "
           "after another\n"
           "  glassbox-dvp-sim --scenario S6 --watch  watch one scenario in "
           "slow motion\n"
           "  glassbox-dvp-sim --scenario S6 --story  the same explanation as "
           "plain text\n"
           "  glassbox-dvp-sim --scenario S6          the raw trace, one line "
           "per event\n"
           "  glassbox-dvp-sim --list                 scenario ids and names\n"
           "  glassbox-dvp-sim --help                 this manual\n"
           "\n"
           "OPTIONS (add to any command above)\n"
           "  --seed N            random seed for network jitter and drops "
           "(default 1)\n"
           "  --speed S           slow, normal, fast, or 25..800 percent "
           "(default normal)\n"
           "  --paused            start paused; press n to step one event\n"
           "  --no-color          plain text, no colors\n"
           "  --set KEY=VALUE     change the simulated system (repeatable):\n"
        << k_settings_help
        << "\n"
           "KEYS\n"
        << k_keys_help
        << "\n"
           "MAKE SHORTCUTS\n"
           "  make watch S=S6 [SPEED=fast] [SEED=7] [SET=\"vote_timeout=30ms "
           "...\"]\n"
           "  make story S=S6    make run S=S6    make list    make manual\n"
           "  make build         make test        make test-all    make help\n"
           "\n"
           "EXIT CODES  0 finished · 2 bad usage · 3 the run did not finish\n"
           "\n"
           "Full manual: docs/MANUAL.md\n";
}

// Prints the narration of `rec` and its end card as plain text.
void print_story_and_summary(const glassbox::ScenarioInfo& info,
                             const cli::Recording& rec,
                             const std::string& title)
{
    std::cout << title << "\n\n";
    print_story(rec);
    if (const cli::ScenarioGuide* guide = cli::find_guide(info.id)) {
        std::cout << '\n';
        for (std::string line :
             cli::draw_summary(rec, *guide, cli::ScreenOptions{100, false})) {
            while (!line.empty() && line.back() == ' ') {
                line.pop_back();
            }
            std::cout << line << '\n';
        }
    }
}

enum class Mode { trace, watch, story };

// Runs scenario `info` with `options` in `mode`. A setting that does not
// apply is reported in `*error` (or on stderr when error is null). Speed
// changes made while watching are kept in `options`. Returns the exit code:
// 0 finished, 2 bad setting, 3 the run did not finish.
int run_scenario(const glassbox::ScenarioInfo& info,
                 Options& options,
                 Mode mode,
                 std::string* error = nullptr)
{
    glassbox::ScenarioSetup setup = info.make(options.seed);
    std::string settings_text;
    for (const std::string& setting : options.settings) {
        const cli::DialStatus status = cli::apply_dial(setup, setting);
        if (status != cli::DialStatus::ok) {
            const std::string message =
                setting + ": " + std::string{cli::to_string(status)};
            if (error) {
                *error = message;
            } else {
                std::cerr << "--set " << message << '\n';
            }
            return 2;
        }
        settings_text += (settings_text.empty() ? " · " : ", ") + setting;
    }

    if (mode == Mode::trace) {
        StdoutSink out;
        glassbox::ScenarioRun run{setup, &out};
        const bool quiesced = run.run();
        print_final_state(run);
        std::cout << "scenario=" << info.id << " seed=" << options.seed
                  << " quiesced=" << (quiesced ? "yes" : "no") << '\n';
        return quiesced ? 0 : 3;
    }

    const cli::Recording rec = cli::record(setup, k_max_player_events);
    const std::string title = title_of(info, options.seed) + settings_text;
    if (mode == Mode::story || !cli::Terminal::interactive()) {
        if (mode == Mode::watch) {
            std::cerr << "not a terminal: printing the story instead\n";
        }
        print_story_and_summary(info, rec, title);
    } else {
        cli::Terminal term;
        Player player{term,          rec,           cli::find_guide(info.id),
                      title,         options.speed, options.paused,
                      options.color};
        static_cast<void>(player.play());
        options.speed = player.speed;
    }
    if (!rec.quiesced) {
        std::cerr << "stopped after " << k_max_player_events
                  << " events: the run did not finish\n";
    }
    return rec.quiesced ? 0 : 3;
}

// Plays the tour scenarios one after another; q leaves the tour. Returns 0,
// or 2 without a terminal.
int run_tour(Options& options)
{
    if (!cli::Terminal::interactive()) {
        std::cerr << "the tour needs a terminal; try --story\n";
        return 2;
    }
    cli::Terminal term;
    for (const std::string_view id : cli::k_tour) {
        const glassbox::ScenarioInfo* info = glassbox::find_scenario(id);
        const cli::Recording rec =
            cli::record(info->make(options.seed), k_max_player_events);
        Player player{term,          rec,           cli::find_guide(id),
                      title_of(*info, options.seed), options.speed,
                      options.paused, options.color};
        const Next next = player.play();
        options.speed = player.speed;
        if (next == Next::quit) {
            break;
        }
    }
    return 0;
}

// --- The how-to-play menu ----------------------------------------------------

// One line typed at the menu prompt.
struct MenuCommand {
    enum class Kind {
        empty,
        watch,    // 1..14, S6, watch 6
        read,     // r 6, read 6, story 6
        tour,     // t, tour
        speed,    // speed fast
        seed,     // seed 7
        set,      // set key=value
        clear,    // clear
        settings, // settings
        keys,     // keys
        help,     // help, h, ?
        quit,     // q, quit, exit
        unknown,
    };
    Kind kind = Kind::empty;
    std::string arg;
};

// Returns "S<n>" for "n" or "Sn"/"sn" (n = 0..99), else "". Whether that
// scenario exists is checked by the caller.
[[nodiscard]] std::string scenario_arg(std::string_view word)
{
    if (!word.empty() && (word.front() == 'S' || word.front() == 's')) {
        word.remove_prefix(1);
    }
    int n = 0;
    const auto [end, error] =
        std::from_chars(word.data(), word.data() + word.size(), n);
    if (word.empty() || error != std::errc{} ||
        end != word.data() + word.size() || n < 0 || n > 99) {
        return {};
    }
    return "S" + std::to_string(n);
}

[[nodiscard]] MenuCommand parse_menu(std::string_view line)
{
    using Kind = MenuCommand::Kind;
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.remove_prefix(1);
    }
    while (!line.empty() &&
           (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    const std::size_t space = line.find(' ');
    std::string word{line.substr(0, space)};
    for (char& c : word) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    std::string_view rest =
        space == std::string_view::npos ? std::string_view{} : line.substr(space + 1);
    while (!rest.empty() && rest.front() == ' ') {
        rest.remove_prefix(1);
    }
    const std::string arg{rest};
    if (word.empty()) {
        return {Kind::empty, {}};
    }
    if (word == "q" || word == "quit" || word == "exit") {
        return {Kind::quit, {}};
    }
    if (word == "t" || word == "tour") {
        return {Kind::tour, {}};
    }
    if (word == "w" || word == "watch") {
        return {Kind::watch, scenario_arg(rest)};
    }
    if (word == "r" || word == "read" || word == "story") {
        return {Kind::read, scenario_arg(rest)};
    }
    if (word == "speed") {
        return {Kind::speed, arg};
    }
    if (word == "seed") {
        return {Kind::seed, arg};
    }
    if (word == "set") {
        return {Kind::set, arg};
    }
    if (word == "clear") {
        return {Kind::clear, {}};
    }
    if (word == "settings") {
        return {Kind::settings, {}};
    }
    if (word == "keys") {
        return {Kind::keys, {}};
    }
    if (word == "h" || word == "help" || word == "?") {
        return {Kind::help, {}};
    }
    if (rest.empty()) {
        if (std::string id = scenario_arg(word); !id.empty()) {
            return {Kind::watch, id};
        }
    }
    return {Kind::unknown, std::string{line}};
}

void print_menu(const Options& options, std::string_view status, bool screen)
{
    if (screen) {
        std::cout << "\x1b[2J\x1b[H"; // clear the terminal
    }
    const bool color = screen && options.color;
    const auto bold = [&](std::string_view text) {
        return color ? "\x1b[1m" + std::string{text} + "\x1b[0m"
                     : std::string{text};
    };
    std::cout
        << bold("GLASS-BOX DvP SETTLEMENT · how to play") << "\n\n"
        << "  Two banks settle a bond trade atomically: each locks its side, "
           "both vote,\n"
           "  and the bonds and the cash move together or not at all, even "
           "when a node\n"
           "  crashes or the network fails. Pick a scenario and watch every "
           "message,\n"
           "  lock, disk write and timer. (A simplified educational model.)\n\n"
        << bold("  Scenarios") << '\n';
    for (const glassbox::ScenarioInfo& info : glassbox::scenario_registry()) {
        const cli::ScenarioGuide* guide = cli::find_guide(info.id);
        const std::string number{info.id.substr(1)};
        std::cout << "    " << std::string(2 - std::min<std::size_t>(2, number.size()), ' ')
                  << number << "  "
                  << (guide ? guide->title : info.name) << '\n';
    }
    std::string settings = "none";
    if (!options.settings.empty()) {
        settings.clear();
        for (const std::string& s : options.settings) {
            settings += (settings.empty() ? "" : ", ") + s;
        }
    }
    std::cout << '\n'
              << "  Now: speed " << speed_name(options.speed) << " · seed "
              << options.seed << " · settings: " << settings << "\n\n"
              << bold("  Type a command, then Enter") << '\n'
              << "    1 ... 14     watch that scenario      t      guided tour "
                 "(1, 2, 6, 9, 14)\n"
                 "    r 6          read scenario 6 as text  keys   keys while "
                 "watching\n"
                 "    speed fast   slow, normal, fast, 25..800 (%)\n"
                 "    seed 7       other random network delays\n"
                 "    set vote_timeout=30ms   change the system (type: "
                 "settings)\n"
                 "    clear        remove all settings      help   every "
                 "command-line flag\n"
                 "    q            quit\n";
    if (!status.empty()) {
        std::cout << '\n' << "  " << status << '\n';
    }
    std::cout << "\n  > " << std::flush;
}

// Waits for Enter (only when someone is at the keyboard).
void wait_for_enter()
{
    std::cout << "\n  Press Enter to go back to the menu." << std::flush;
    std::string ignored;
    static_cast<void>(std::getline(std::cin, ignored));
}

// The how-to-play loop: show the menu, run what was asked, come back.
// Returns 0 when the user quits (or input ends).
int run_menu(Options options)
{
    const bool screen = cli::Terminal::interactive();
    std::string status = "Type 1 and press Enter to start with the happy path.";
    while (true) {
        print_menu(options, status, screen);
        status.clear();
        std::string line;
        if (!std::getline(std::cin, line)) {
            std::cout << '\n';
            return 0;
        }
        if (!screen) {
            std::cout << line << '\n'; // echo piped input
        }
        const MenuCommand command = parse_menu(line);
        using Kind = MenuCommand::Kind;
        switch (command.kind) {
        case Kind::empty:
            break;
        case Kind::quit:
            return 0;
        case Kind::watch:
        case Kind::read: {
            const glassbox::ScenarioInfo* info =
                command.arg.empty() ? nullptr
                                    : glassbox::find_scenario(command.arg);
            if (!info) {
                status = "There is no such scenario. Pick a number from 1 to " +
                         std::to_string(glassbox::scenario_registry().size()) +
                         ".";
                break;
            }
            std::string error;
            const bool read = command.kind == Kind::read;
            const int code = run_scenario(*info, options,
                                          read ? Mode::story : Mode::watch,
                                          &error);
            if (code == 2) {
                status = "Could not start " + std::string{info->id} + ": " +
                         error + ". Type clear to remove settings.";
                break;
            }
            if (read || !screen) {
                wait_for_enter();
            }
            status = "Finished " + std::string{info->id} +
                     ". Pick another scenario, or q to quit.";
            break;
        }
        case Kind::tour:
            if (run_tour(options) != 0) {
                status = "The tour needs a terminal.";
            } else {
                status = "Tour finished. Pick a scenario, or q to quit.";
            }
            break;
        case Kind::speed:
            if (const auto speed = parse_speed(command.arg)) {
                options.speed = *speed;
                status = "Speed is now " + speed_name(*speed) + ".";
            } else {
                status = "Speed must be slow, normal, fast, or 25 to 800.";
            }
            break;
        case Kind::seed:
            if (const auto seed = parse_seed(command.arg)) {
                options.seed = *seed;
                status = "Seed is now " + std::to_string(*seed) + ".";
            } else {
                status = "The seed must be a whole number, e.g. seed 7.";
            }
            break;
        case Kind::set: {
            // Check the syntax on S6, which accepts every setting.
            glassbox::ScenarioSetup probe =
                glassbox::find_scenario("S6")->make(options.seed);
            const cli::DialStatus check = cli::apply_dial(probe, command.arg);
            if (check == cli::DialStatus::ok) {
                options.settings.push_back(command.arg);
                status = "Setting added: " + command.arg + ".";
            } else {
                status = command.arg + ": " + std::string{cli::to_string(check)} +
                         ". Type settings to see what you can set.";
            }
            break;
        }
        case Kind::clear:
            options.settings.clear();
            status = "Settings cleared.";
            break;
        case Kind::settings:
            std::cout << '\n' << k_settings_help;
            wait_for_enter();
            break;
        case Kind::keys:
            std::cout << '\n' << k_keys_help;
            wait_for_enter();
            break;
        case Kind::help:
            std::cout << '\n';
            print_manual(std::cout);
            wait_for_enter();
            break;
        case Kind::unknown:
            status = "Unknown command: \"" + command.arg +
                     "\". Type a number from 1 to 14, t for the tour, or q.";
            break;
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::optional<std::string_view> scenario_id;
    Options options;
    bool watch = false;
    bool story = false;
    bool tour = false;
    bool menu = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--list") {
            for (const glassbox::ScenarioInfo& info :
                 glassbox::scenario_registry()) {
                std::cout << info.id << "  " << info.name << '\n';
            }
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            print_manual(std::cout);
            return 0;
        }
        if (arg == "--scenario" && i + 1 < argc) {
            scenario_id = argv[++i];
        } else if (arg == "--seed" && i + 1 < argc) {
            const auto parsed = parse_seed(argv[++i]);
            if (!parsed) {
                return usage();
            }
            options.seed = *parsed;
        } else if (arg == "--watch") {
            watch = true;
        } else if (arg == "--story") {
            story = true;
        } else if (arg == "--tour") {
            tour = true;
        } else if (arg == "--menu") {
            menu = true;
        } else if (arg == "--set" && i + 1 < argc) {
            options.settings.emplace_back(argv[++i]);
        } else if (arg == "--paused") {
            options.paused = true;
        } else if (arg == "--no-color") {
            options.color = false;
        } else if (arg == "--speed" && i + 1 < argc) {
            const auto parsed = parse_speed(argv[++i]);
            if (!parsed) {
                return usage();
            }
            options.speed = *parsed;
        } else {
            return usage();
        }
    }
    const int modes = (scenario_id ? 1 : 0) + (tour ? 1 : 0) + (menu ? 1 : 0);
    if (modes > 1 || (watch && story) ||
        ((watch || story) && !scenario_id)) {
        return usage();
    }
    if (tour) {
        return options.settings.empty() ? run_tour(options) : usage();
    }
    if (!scenario_id) {
        // No scenario: the menu for a person at a terminal, else the manual.
        if (menu || (cli::Terminal::interactive())) {
            return run_menu(options);
        }
        print_manual(std::cout);
        return 0;
    }
    const glassbox::ScenarioInfo* info = glassbox::find_scenario(*scenario_id);
    if (!info) {
        std::cerr << "unknown scenario: " << *scenario_id << '\n';
        return 2;
    }
    return run_scenario(*info, options,
                        watch ? Mode::watch
                              : (story ? Mode::story : Mode::trace));
}
