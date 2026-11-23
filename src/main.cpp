// glassbox-dvp-sim command-line runner.
//
//   glassbox-dvp-sim --list
//   glassbox-dvp-sim --scenario S1 [--seed 1]            full trace at once
//   glassbox-dvp-sim --scenario S1 --watch [options]     glass-box player
//   glassbox-dvp-sim --scenario S1 --story               narrated, plain text
//   glassbox-dvp-sim --tour [options]                    S1, S2, S6, S9, S14
//
// Player options: --speed slow|normal|fast|<25..800>, --paused, --no-color.
// Exit codes: 0 quiesced, 2 usage error, 3 event bound hit (not quiesced).
//
// The player records the run first (cli/recording) and then only paces
// what it shows; the simulation itself never waits or slows down.

#include "cli/guide.hpp"
#include "cli/narrator.hpp"
#include "cli/pacing.hpp"
#include "cli/recording.hpp"
#include "cli/screen.hpp"
#include "cli/terminal.hpp"
#include "scenarios/scenario.hpp"
#include "trace/trace_format.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

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

int usage()
{
    std::cerr
        << "usage: glassbox-dvp-sim --list\n"
           "       glassbox-dvp-sim --scenario <ID> [--seed <N>]\n"
           "       glassbox-dvp-sim --scenario <ID> --watch [--speed "
           "slow|normal|fast|<25..800>] [--paused] [--no-color]\n"
           "       glassbox-dvp-sim --scenario <ID> --story\n"
           "       glassbox-dvp-sim --tour [--speed ...] [--no-color]\n";
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
                "(starts by itself in a few seconds)");
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
        // End: hold the last event until a key (or k_card_ms).
        render(rec.frames.size() - 1, rec.frames.back().time_us);
        for (std::int64_t waited = 0; waited < k_card_ms * 100 / speed;
             waited += 250) {
            const cli::Key key = term.wait_key(250);
            if (key == cli::Key::restart) {
                return play();
            }
            if (key == cli::Key::quit) {
                return Next::quit;
            }
            if (key != cli::Key::none) {
                break;
            }
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

} // namespace

int main(int argc, char** argv)
{
    std::optional<std::string_view> scenario_id;
    std::uint64_t seed = 1;
    bool watch = false;
    bool story = false;
    bool start_paused = false;
    bool color = true;
    bool tour = false;
    int speed = cli::k_default_speed_percent;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--list") {
            for (const glassbox::ScenarioInfo& info :
                 glassbox::scenario_registry()) {
                std::cout << info.id << "  " << info.name << '\n';
            }
            return 0;
        }
        if (arg == "--scenario" && i + 1 < argc) {
            scenario_id = argv[++i];
        } else if (arg == "--seed" && i + 1 < argc) {
            const auto parsed = parse_seed(argv[++i]);
            if (!parsed) {
                return usage();
            }
            seed = *parsed;
        } else if (arg == "--watch") {
            watch = true;
        } else if (arg == "--story") {
            story = true;
        } else if (arg == "--tour") {
            tour = true;
        } else if (arg == "--paused") {
            start_paused = true;
        } else if (arg == "--no-color") {
            color = false;
        } else if (arg == "--speed" && i + 1 < argc) {
            const auto parsed = parse_speed(argv[++i]);
            if (!parsed) {
                return usage();
            }
            speed = *parsed;
        } else {
            return usage();
        }
    }
    if (tour) {
        if (scenario_id || watch || story) {
            return usage();
        }
        if (!cli::Terminal::interactive()) {
            std::cerr << "the tour needs a terminal; try --story\n";
            return 2;
        }
        cli::Terminal term;
        for (const std::string_view id : cli::k_tour) {
            const glassbox::ScenarioInfo* info = glassbox::find_scenario(id);
            const cli::Recording rec =
                cli::record(info->make(seed), k_max_player_events);
            Player player{term,  rec,          cli::find_guide(id),
                          title_of(*info, seed), speed, start_paused, color};
            if (player.play() == Next::quit) {
                break;
            }
            speed = player.speed;
        }
        return 0;
    }
    if (!scenario_id || (watch && story)) {
        return usage();
    }
    const glassbox::ScenarioInfo* info = glassbox::find_scenario(*scenario_id);
    if (!info) {
        std::cerr << "unknown scenario: " << *scenario_id << '\n';
        return 2;
    }

    if (watch || story) {
        const cli::Recording rec =
            cli::record(info->make(seed), k_max_player_events);
        const std::string title = title_of(*info, seed);
        if (story || !cli::Terminal::interactive()) {
            if (watch) {
                std::cerr << "not a terminal: printing the story instead\n";
            }
            std::cout << title << "\n\n";
            print_story(rec);
        } else {
            cli::Terminal term;
            Player player{term,  rec,   cli::find_guide(info->id),
                          title, speed, start_paused, color};
            static_cast<void>(player.play());
        }
        if (!rec.quiesced) {
            std::cerr << "stopped after " << k_max_player_events
                      << " events: the run did not finish\n";
        }
        return rec.quiesced ? 0 : 3;
    }

    StdoutSink out;
    glassbox::ScenarioRun run{info->make(seed), &out};
    const bool quiesced = run.run();
    print_final_state(run);
    std::cout << "scenario=" << info->id << " seed=" << seed
              << " quiesced=" << (quiesced ? "yes" : "no") << '\n';
    return quiesced ? 0 : 3;
}
