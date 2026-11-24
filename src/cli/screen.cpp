// Text layout of the player screen, the intro card and the end card. Lines
// are laid out as plain text and colored last, so widths are exact; every
// character used is one column wide.

#include "cli/screen.hpp"

#include "cli/narrator.hpp"
#include "cli/summary.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>

namespace glassbox::cli {

namespace {

// --- Text helpers (plain text in, color added last) -------------------------

[[nodiscard]] std::string fit(std::string_view text, std::size_t width)
{
    std::string out;
    std::size_t cols = 0;
    for (std::size_t i = 0; i < text.size();) {
        std::size_t len = 1;
        const auto c = static_cast<unsigned char>(text[i]);
        if (c >= 0xF0) {
            len = 4;
        } else if (c >= 0xE0) {
            len = 3;
        } else if (c >= 0xC0) {
            len = 2;
        }
        if (cols + 1 > width) {
            break;
        }
        if (cols + 1 == width && i + len < text.size()) {
            out += "…";
            ++cols;
            break;
        }
        out.append(text.substr(i, len));
        ++cols;
        i += len;
    }
    out.append(width - cols, ' ');
    return out;
}

[[nodiscard]] std::string repeat(std::string_view piece, std::size_t n)
{
    std::string out;
    for (std::size_t i = 0; i < n; ++i) {
        out += piece;
    }
    return out;
}

[[nodiscard]] std::string right(std::string_view text, std::size_t width)
{
    const std::size_t w = visible_width(text);
    return w >= width ? std::string{text}
                      : std::string(width - w, ' ') + std::string{text};
}

// Word-wrap `text` to `width`, the first line after `first`, the rest after
// `rest` (both prefixes are plain and equally wide).
[[nodiscard]] std::vector<std::string> wrap(std::string_view text,
                                            std::size_t width,
                                            std::string_view first,
                                            std::string_view rest)
{
    std::vector<std::string> lines;
    const std::size_t room = width > visible_width(first)
                                 ? width - visible_width(first)
                                 : 1;
    std::string line;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find(' ', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view word = text.substr(start, end - start);
        if (!line.empty() &&
            visible_width(line) + 1 + visible_width(word) > room) {
            lines.push_back(line);
            line.clear();
        }
        if (!line.empty()) {
            line += ' ';
        }
        line += word;
        start = end + 1;
    }
    lines.push_back(line);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        lines[i] = std::string{i == 0 ? first : rest} + lines[i];
    }
    return lines;
}

struct Paint {
    bool on = true;
    [[nodiscard]] std::string operator()(std::string_view text,
                                         std::string_view code) const
    {
        if (!on || text.empty()) {
            return std::string{text};
        }
        return "\x1b[" + std::string{code} + "m" + std::string{text} +
               "\x1b[0m";
    }
};

constexpr std::string_view k_bold = "1";
constexpr std::string_view k_dim = "2";
constexpr std::string_view k_red = "31";
constexpr std::string_view k_green = "32";
constexpr std::string_view k_yellow = "33";
constexpr std::string_view k_cyan = "36";

[[nodiscard]] std::string_view tone_color(Tone tone)
{
    switch (tone) {
    case Tone::good:
        return k_green;
    case Tone::warning:
        return k_yellow;
    case Tone::failure:
        return k_red;
    case Tone::info:
        break;
    }
    return k_bold;
}

[[nodiscard]] std::string_view tone_mark(Tone tone)
{
    switch (tone) {
    case Tone::good:
        return "✓";
    case Tone::warning:
        return "!";
    case Tone::failure:
        return "✗";
    case Tone::info:
        break;
    }
    return "▶";
}

// --- Facts derived from the recording ----------------------------------------

[[nodiscard]] std::string name(NodeId node)
{
    return std::string{glassbox::to_string(node)};
}

[[nodiscard]] std::string letter(NodeId node)
{
    const std::string n = name(node);
    return n.substr(n.size() - 1);
}

[[nodiscard]] std::vector<NodeId> involved_nodes(const Recording& rec)
{
    std::set<NodeId> seen{NodeId::coordinator};
    for (const Frame& f : rec.frames) {
        for (const TraceRecord& r : f.records) {
            for (const auto& node : {r.node, r.from, r.to}) {
                if (node) {
                    seen.insert(*node);
                }
            }
        }
    }
    std::vector<NodeId> out;
    for (const NodeId node : k_view_order) {
        if (seen.contains(node)) {
            out.push_back(node);
        }
    }
    return out;
}

[[nodiscard]] std::optional<Trade> trade_terms(const Recording& rec, TradeId id)
{
    for (const Frame& f : rec.frames) {
        const auto& coord = f.nodes[0].coordinator_trades;
        if (const auto it = coord.find(id); it != coord.end()) {
            return it->second.trade;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<TradeId> frame_trade(const Frame& f)
{
    for (const TraceRecord& r : f.records) {
        if (r.trade_id) {
            return r.trade_id;
        }
    }
    return std::nullopt;
}

// Last crash time of `node` up to frame `upto`, and whether it is still
// crashed (no restart after it).
struct CrashState {
    std::optional<SimTimeUs> last_crash;
    bool crashed = false;
};

[[nodiscard]] CrashState crash_state(const Recording& rec,
                                     std::size_t upto,
                                     NodeId node)
{
    CrashState s;
    for (std::size_t i = 1; i <= upto && i < rec.frames.size(); ++i) {
        for (const TraceRecord& r : rec.frames[i].records) {
            if (r.node != node) {
                continue;
            }
            if (r.kind == "CRASH") {
                s.last_crash = r.sim_time_us;
                s.crashed = true;
            } else if (r.kind == "RESTART") {
                s.crashed = false;
            }
        }
    }
    return s;
}

[[nodiscard]] std::string timer_short(std::string_view kind)
{
    if (kind == "VOTE_TIMEOUT") {
        return "vote";
    }
    if (kind == "RETRANSMIT") {
        return "resend";
    }
    if (kind == "DECISION_WAIT") {
        return "ask";
    }
    return std::string{kind};
}

[[nodiscard]] std::string state_label(TradeState state)
{
    switch (state) {
    case TradeState::preparing:
        return "PREPARING (writing)";
    case TradeState::prepared:
        return "PREPARED (voted YES)";
    case TradeState::committed:
        return "COMMITTED";
    case TradeState::aborted:
        return "ABORTED";
    default:
        return std::string{glassbox::to_string(state)};
    }
}

// --- Sections ---------------------------------------------------------------

[[nodiscard]] std::vector<std::string> trade_lines(const Recording& rec,
                                                   const Moment& m,
                                                   std::size_t width,
                                                   const Paint& paint)
{
    // Stage reached per trade so far, from the stories of past events.
    std::map<TradeId, int> reached;
    for (std::size_t i = 1; i <= m.frame && i < rec.frames.size(); ++i) {
        const auto id = frame_trade(rec.frames[i]);
        if (!id) {
            continue;
        }
        const Story s = narrate(rec, i);
        int& best = reached.try_emplace(*id, -1).first->second;
        if (s.stage) {
            best = std::max(best, static_cast<int>(*s.stage));
        }
    }
    std::vector<std::string> out;
    const Frame& now = rec.frames[m.frame];
    for (const auto& [id, stage] : reached) {
        const auto trade = trade_terms(rec, id);
        if (!trade) {
            continue;
        }
        const std::string ticket =
            "#" + std::to_string(id.value()) + "  " + name(trade->buyer()) +
            " buys " + bonds_text(trade->qty().value()) + " from " +
            name(trade->seller()) + " for " + dollars(trade->cash_cents());
        out.push_back(paint(fit(ticket, width), k_bold));

        const auto& coord = now.nodes[0].coordinator_trades;
        const auto it = coord.find(id);
        const bool done = it != coord.end() && it->second.state == TradeState::done;
        std::string bar = "    ";
        std::size_t cols = 4;
        for (int s = 0; s <= static_cast<int>(Stage::confirm); ++s) {
            const std::string label{to_string(static_cast<Stage>(s))};
            std::string mark = "○";
            std::string_view color = k_dim;
            if (done || s < stage) {
                mark = "✓";
                color = k_green;
            } else if (s == stage) {
                mark = "●";
                color = k_yellow;
            }
            if (s > 0) {
                bar += " ─ ";
                cols += 3;
            }
            bar += paint(mark + " " + label, color);
            cols += 2 + label.size();
        }
        if (done && it->second.decision) {
            const bool commit = *it->second.decision == Decision::commit;
            std::string outcome = commit ? "   SETTLED: both legs moved"
                                         : "   CANCELLED: nothing moved";
            if (cols + visible_width(outcome) > width) {
                // The short form always fits: the bar is 67 columns, the
                // screen at least 80.
                outcome = commit ? "   SETTLED" : "   CANCELLED";
            }
            bar += paint(outcome, commit ? k_green : k_red);
            cols += visible_width(outcome);
        }
        if (cols < width) {
            bar.append(width - cols, ' ');
        }
        out.push_back(bar);
    }
    if (out.empty()) {
        out.push_back(fit("(no trade submitted yet)", width));
    }
    return out;
}

[[nodiscard]] std::vector<std::string> panel_body(const Recording& rec,
                                                  const Moment& m,
                                                  NodeId node,
                                                  std::size_t inner)
{
    const Frame& f = rec.frames[m.frame];
    const NodeView& v = f.nodes[view_index(node)];
    const CrashState crash = crash_state(rec, m.frame, node);
    std::vector<std::string> lines;
    const auto amount = [&](std::string_view label, const std::string& value) {
        const std::size_t room = inner > label.size() ? inner - label.size() : 0;
        return std::string{label} + right(value, room);
    };

    if (crash.crashed) {
        lines.push_back("✗ DOWN: memory lost");
    } else if (!v.up) {
        lines.push_back("✗ OFFLINE: gets no messages");
    }
    if (!crash.crashed) {
        if (v.balances) {
            const BalanceView& b = *v.balances;
            lines.push_back(amount("cash", dollars(Cents{b.cash_total - b.cash_locked})));
            if (b.cash_locked > 0) {
                lines.push_back(amount("  locked", dollars(Cents{b.cash_locked})));
            }
            lines.push_back(amount("bonds", with_commas(b.bonds_total - b.bonds_locked)));
            if (b.bonds_locked > 0) {
                lines.push_back(amount("  locked", with_commas(b.bonds_locked)));
            }
            if (v.frozen) {
                lines.push_back("account FROZEN");
            }
            for (const auto& [id, trade] : v.trades) {
                lines.push_back("#" + std::to_string(id.value()) + " " +
                                state_label(trade.state));
            }
        } else {
            for (const auto& [id, t] : v.coordinator_trades) {
                const std::string buyer = letter(t.trade.buyer());
                const std::string seller = letter(t.trade.seller());
                lines.push_back("#" + std::to_string(id.value()) + " " +
                                std::string{glassbox::to_string(t.state)} +
                                (t.decision && t.state == TradeState::done
                                     ? " (" +
                                           std::string{glassbox::to_string(
                                               *t.decision)} +
                                           ")"
                                     : ""));
                if (t.state == TradeState::collecting) {
                    lines.push_back("  votes " + buyer +
                                    (t.buyer_voted_yes ? " ✓  " : " ·  ") +
                                    seller + (t.seller_voted_yes ? " ✓" : " ·"));
                } else if (t.state != TradeState::done) {
                    lines.push_back(t.decision_durable ? "  decision on disk ✓"
                                                       : "  writing decision…");
                    lines.push_back("  acks  " + buyer +
                                    (t.buyer_acked ? " ✓  " : " ·  ") + seller +
                                    (t.seller_acked ? " ✓" : " ·"));
                }
            }
            if (v.coordinator_trades.empty()) {
                lines.push_back("(no trades yet)");
            }
        }
        // Timers still pending at this moment (none while down).
        if (v.up) {
            for (const TimerSpan& t : rec.timers) {
                const bool current =
                    !crash.last_crash || t.set_at_us >= *crash.last_crash;
                const bool pending =
                    !t.fires_at_us || *t.fires_at_us > m.now_us;
                if (t.node == node && current && t.set_at_us <= m.now_us &&
                    pending) {
                    lines.push_back(
                        "timer " + timer_short(t.kind) + " @" +
                        (t.fires_at_us ? format_time(*t.fires_at_us) : "?"));
                }
            }
        }
    }
    if (v.disk.empty()) {
        lines.push_back("disk  (empty)");
    } else {
        lines.push_back("disk");
        const std::size_t from = v.disk.size() > 4 ? v.disk.size() - 4 : 0;
        if (from > 0) {
            lines.push_back("  … " + std::to_string(from) + " earlier");
        }
        for (std::size_t i = from; i < v.disk.size(); ++i) {
            lines.push_back("  " + v.disk[i]);
        }
    }
    return lines;
}

[[nodiscard]] std::vector<std::string> panel_rows(const Recording& rec,
                                                  const Moment& m,
                                                  const std::vector<NodeId>& nodes,
                                                  std::size_t width,
                                                  const Paint& paint)
{
    const std::size_t n = nodes.size();
    const std::size_t outer = (width - (n - 1)) / n;
    const std::size_t inner = outer - 4;
    std::vector<std::vector<std::string>> bodies;
    std::size_t height = 0;
    for (const NodeId node : nodes) {
        bodies.push_back(panel_body(rec, m, node, inner));
        height = std::max(height, bodies.back().size());
    }
    std::vector<std::string> rows(height + 2);
    for (std::size_t p = 0; p < n; ++p) {
        const NodeId node = nodes[p];
        const CrashState crash = crash_state(rec, m.frame, node);
        const bool down = !rec.frames[m.frame].nodes[view_index(node)].up;
        const std::string title = " " + name(node) + " · " +
                                  std::string{city(node)} + " ";
        const std::size_t title_cols = visible_width(title);
        const std::size_t fill = outer > title_cols + 3 ? outer - title_cols - 3 : 0;
        const std::string sep = p + 1 < n ? " " : "";
        rows[0] += "┌─" +
                   paint(title, down || crash.crashed ? k_red : k_bold) +
                   repeat("─", fill) + "┐" + sep;
        for (std::size_t r = 0; r < height; ++r) {
            const std::string text =
                r < bodies[p].size() ? bodies[p][r] : std::string{};
            std::string cell = fit(text, inner);
            if (text.starts_with("✗")) {
                cell = paint(cell, k_red);
            } else if (text.starts_with("disk") || text.starts_with("timer")) {
                cell = paint(cell, k_cyan);
            }
            rows[r + 1] += "│ " + cell + " │" + sep;
        }
        rows[height + 1] += "└" + repeat("─", outer - 2) + "┘" + sep;
    }
    return rows;
}

[[nodiscard]] std::string flight_label(const Flight& f)
{
    switch (f.type) {
    case MessageType::vote:
        return f.detail && f.detail->starts_with("NO") ? "VOTE NO" : "VOTE YES";
    case MessageType::outcome_query:
        return "QUERY";
    default:
        return std::string{glassbox::to_string(f.type)};
    }
}

[[nodiscard]] std::vector<std::string> wire_lines(const Recording& rec,
                                                  const Moment& m,
                                                  std::size_t width,
                                                  const Paint& paint)
{
    std::vector<std::string> out;
    std::string header = "On the wire";
    if (m.frame + 1 < rec.frames.size() &&
        m.now_us > rec.frames[m.frame].time_us) {
        header += "   · next event in " +
                  format_duration(rec.frames[m.frame + 1].time_us - m.now_us);
    }
    out.push_back(paint(fit(header, width), k_bold));
    const std::vector<const Flight*> flights = in_flight(rec, m.now_us);
    const std::size_t bar = width > 74 ? width - 66 : 8;
    std::size_t shown = 0;
    for (const Flight* f : flights) {
        if (shown == 6) {
            out.push_back(fit("  … and " + std::to_string(flights.size() - 6) +
                                  " more",
                              width));
            break;
        }
        ++shown;
        std::size_t pos = 0;
        std::string duration = "?";
        if (f->ends_at_us != k_open_end && f->ends_at_us > f->sent_at_us) {
            const SimTimeUs total = f->ends_at_us - f->sent_at_us;
            // in_flight() gives sent_at <= now < ends_at, so done < total
            // and pos < bar - 1.
            const SimTimeUs done = m.now_us - f->sent_at_us;
            const auto scaled =
                checked_multiply_i64(done, static_cast<SimTimeUs>(bar - 1));
            // Only a flight longer than ~1,000 years of sim time overflows;
            // then the dot stays at the start.
            pos = scaled ? static_cast<std::size_t>(*scaled / total) : 0;
            duration = format_duration(total);
        }
        const std::string link =
            city(f->from) == city(f->to)
                ? "within " + std::string{city(f->from)}
                : std::string{city(f->from)} + " → " + std::string{city(f->to)};
        std::string line = "  " + fit(flight_label(*f), 10) + " " +
                           right(name(f->from), 6) + " " + repeat("─", pos) +
                           paint("●", k_yellow) + repeat("─", bar - 1 - pos) +
                           "▶ " + fit(name(f->to), 6) + " " + right(duration, 9) +
                           "  " + link;
        const std::size_t cols = visible_width(line);
        if (cols < width) {
            line.append(width - cols, ' ');
        }
        out.push_back(line);
    }
    if (flights.empty()) {
        out.push_back(fit("  (nothing on the wire)", width));
    }
    return out;
}

[[nodiscard]] std::vector<std::string> story_lines(const Recording& rec,
                                                   const Moment& m,
                                                   std::size_t width,
                                                   const Paint& paint)
{
    const Story s = narrate(rec, m.frame);
    std::vector<std::string> out;
    const std::string mark{tone_mark(s.tone)};
    for (const std::string& line :
         wrap(s.headline, width, mark + " ", "  ")) {
        out.push_back(paint(fit(line, width), tone_color(s.tone)));
    }
    for (const std::string& d : s.details) {
        for (const std::string& line : wrap(d, width, "   · ", "     ")) {
            out.push_back(fit(line, width));
        }
    }
    if (!s.why.empty()) {
        for (const std::string& line : wrap(s.why, width, "   why  ", "        ")) {
            out.push_back(paint(fit(line, width), k_cyan));
        }
    }
    if (!s.code.empty()) {
        for (const std::string& line : wrap(s.code, width, "   code ", "        ")) {
            out.push_back(paint(fit(line, width), k_dim));
        }
    }
    return out;
}

[[nodiscard]] std::string check_line(const Recording& rec,
                                     const Moment& m,
                                     std::size_t width,
                                     const Paint& paint)
{
    const auto& violations = rec.frames[m.frame].violations;
    if (violations.empty()) {
        return paint(fit("✓ money conserved   ✓ no negative balances   "
                         "✓ locks match open trades",
                         width),
                     k_green);
    }
    std::string text = "✗ invariant broken:";
    for (const InvariantViolation& v : violations) {
        text += " " + std::string{glassbox::to_string(v.kind)};
    }
    return paint(fit(text, width), k_red);
}

} // namespace

std::size_t visible_width(std::string_view text) noexcept
{
    std::size_t cols = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == 0x1b) {
            // Skip an ANSI escape: ESC [ ... final byte in @..~.
            ++i;
            if (i < text.size() && text[i] == '[') {
                ++i;
                while (i < text.size() &&
                       !(text[i] >= '@' && text[i] <= '~')) {
                    ++i;
                }
            }
            continue;
        }
        if ((c & 0xC0) != 0x80) {
            ++cols;
        }
    }
    return cols;
}

std::vector<std::string> draw_intro(const Recording& rec,
                                    const ScenarioGuide& guide,
                                    const ScreenOptions& options)
{
    const std::size_t width =
        static_cast<std::size_t>(std::clamp(options.width, 80, 200));
    const Paint paint{options.color};
    std::vector<std::string> out;
    const auto section = [&](std::string_view heading) {
        out.push_back(std::string(width, ' '));
        out.push_back(paint(fit("  " + std::string{heading}, width), k_bold));
    };
    out.push_back(paint(fit("  " + std::string{guide.id} + " · " +
                                std::string{guide.title},
                            width),
                        k_bold));

    section("What happens");
    for (const std::string& line :
         wrap(guide.what_it_shows, width - 2, "    ", "    ")) {
        out.push_back(fit(line, width));
    }

    section(rec.frames.back().nodes[0].coordinator_trades.size() > 1
                ? "The trades"
                : "The trade");
    for (const auto& [id, t] : rec.frames.back().nodes[0].coordinator_trades) {
        out.push_back(fit("    #" + std::to_string(id.value()) + "  " +
                              name(t.trade.buyer()) + " buys " +
                              bonds_text(t.trade.qty().value()) + " from " +
                              name(t.trade.seller()) + " for " +
                              dollars(t.trade.cash_cents()),
                          width));
    }

    section("The nodes");
    for (const NodeId node : involved_nodes(rec)) {
        const NodeView& v = rec.frames[0].nodes[view_index(node)];
        std::string line = "    " + fit(name(node), 8) + fit(city(node), 11);
        if (v.balances) {
            line += "cash " + dollars(Cents{v.balances->cash_total}) +
                    " · bonds " + with_commas(v.balances->bonds_total);
            if (v.frozen) {
                line += " · account FROZEN";
            }
        } else {
            line += "coordinator: collects votes, decides, tells both banks";
        }
        out.push_back(fit(line, width));
    }

    section("Watch for");
    for (const std::string& line :
         wrap(guide.watch_for, width - 2, "    ", "    ")) {
        out.push_back(paint(fit(line, width), k_cyan));
    }

    section("How it works");
    for (const std::string& line : wrap(
             "Two-phase commit: each bank checks and locks its side, then "
             "votes. Only if both vote YES does the coordinator write COMMIT "
             "to disk and tell them to settle; otherwise everyone releases. "
             "Either both legs move or neither does.",
             width - 2, "    ", "    ")) {
        out.push_back(fit(line, width));
    }
    return out;
}

std::vector<std::string> draw_summary(const Recording& rec,
                                      const ScenarioGuide& guide,
                                      const ScreenOptions& options)
{
    const std::size_t width =
        static_cast<std::size_t>(std::clamp(options.width, 80, 200));
    const Paint paint{options.color};
    const RunSummary sum = summarize(rec);
    std::vector<std::string> out;
    const auto section = [&](std::string_view heading) {
        out.push_back(std::string(width, ' '));
        out.push_back(paint(fit("  " + std::string{heading}, width), k_bold));
    };
    const auto line = [&](const std::string& text, std::string_view color = {},
                          std::string_view indent = "    ") {
        const std::string rest = std::string{indent} + "  ";
        for (const std::string& l : wrap(text, width - 2, indent, rest)) {
            out.push_back(color.empty() ? fit(l, width) : paint(fit(l, width), color));
        }
    };
    out.push_back(paint(fit("  " + std::string{guide.id} + " · " +
                                std::string{guide.title} +
                                (rec.quiesced ? " · finished"
                                              : " · stopped before the end"),
                            width),
                        k_bold));

    section(sum.trades.size() > 1 ? "Outcomes" : "Outcome");
    for (const TradeOutcome& t : sum.trades) {
        const std::string bonds = bonds_text(t.qty);
        const std::string cash = dollars(t.cash);
        const std::string id = "#" + std::to_string(t.trade_id.value()) + "  ";
        if (t.decision == Decision::commit) {
            line(id + "SETTLED: " + name(t.buyer) + " paid " + cash +
                     " and received " + bonds + "; " + name(t.seller) +
                     " delivered them and received " + cash,
                 k_green);
        } else {
            std::string why{glassbox::to_string(t.reason)};
            for (char& c : why) {
                c = c == '_' ? ' ' : static_cast<char>(std::tolower(c));
            }
            line(id + "CANCELLED (" + why + "): nothing moved at " +
                     name(t.buyer) + " or " + name(t.seller),
                 k_red);
        }
        line("finished " + format_duration(t.settled_after_us) +
                 " after it was submitted",
             {}, "        ");
    }
    if (sum.trades.empty()) {
        line("no trade was decided");
    }

    section("Balances, start → end");
    const auto change = [](const std::string& a, const std::string& b) {
        return a == b ? a + " (unchanged)" : a + " → " + b;
    };
    for (const NodeId node : involved_nodes(rec)) {
        const NodeView& start = rec.frames.front().nodes[view_index(node)];
        const NodeView& end = rec.frames.back().nodes[view_index(node)];
        if (!start.balances || !end.balances) {
            continue;
        }
        line(fit(name(node), 8) + "cash " +
             change(dollars(Cents{start.balances->cash_total}),
                    dollars(Cents{end.balances->cash_total})) +
             "   bonds " +
             change(with_commas(start.balances->bonds_total),
                    with_commas(end.balances->bonds_total)));
    }

    section("Timing (simulated)");
    std::string locks = "assets locked: ";
    bool first = true;
    for (const NodeId node : involved_nodes(rec)) {
        if (node == NodeId::coordinator) {
            continue;
        }
        locks += (first ? "" : " · ") + name(node) + " " +
                 format_duration(sum.lock_hold_us[view_index(node) - 1]);
        first = false;
    }
    line(locks);
    line("longest wait in doubt (voted YES, outcome unknown): " +
         format_duration(sum.longest_in_doubt_us));

    section("Network");
    line(std::to_string(sum.sent) + " messages sent · " +
         std::to_string(sum.dropped) + " lost · " +
         std::to_string(sum.retransmits) + " resent by the coordinator");

    section("Safety");
    // Atomicity: no trade COMMITTED at one bank and not at the other.
    bool agree = true;
    for (const TradeOutcome& t : sum.trades) {
        const auto state = [&](NodeId bank) {
            const auto& trades = rec.frames.back().nodes[view_index(bank)].trades;
            const auto it = trades.find(t.trade_id);
            return it == trades.end() ? TradeState::none : it->second.state;
        };
        agree = agree && ((state(t.buyer) == TradeState::committed) ==
                          (state(t.seller) == TradeState::committed));
    }
    line(std::to_string(sum.checks_run) + " events, invariants checked after "
         "each: " + std::to_string(sum.violations) +
             (sum.violations == 0 ? " problems ✓" : " problems ✗"),
         sum.violations == 0 ? k_green : k_red);
    line(agree ? "both banks agree on every trade: atomic ✓"
               : "the banks disagree on a trade ✗",
         agree ? k_green : k_red);
    return out;
}

std::vector<std::string> draw(const Recording& rec,
                              const Moment& moment,
                              const ScreenOptions& options)
{
    const std::size_t width =
        static_cast<std::size_t>(std::clamp(options.width, 80, 200));
    const Paint paint{options.color};
    Moment m = moment;
    m.frame = std::min(m.frame, rec.frames.size() - 1);
    // "Now" lies between this event and the next one.
    m.now_us = std::max(m.now_us, rec.frames[m.frame].time_us);
    if (m.frame + 1 < rec.frames.size()) {
        m.now_us = std::min(m.now_us, rec.frames[m.frame + 1].time_us);
    }

    std::vector<std::string> out;
    const std::string clock = "t = " + format_time(m.now_us);
    const std::string status =
        std::string{m.paused ? "❚❚ paused" : "▶ playing"} + " · speed " +
        std::to_string(m.speed_percent) + "% · event " +
        std::to_string(m.frame) + "/" + std::to_string(rec.frames.size() - 1);
    const std::size_t gap = width > visible_width(clock) + visible_width(status)
                                ? width - visible_width(clock) - visible_width(status)
                                : 1;
    out.push_back(paint(clock, k_bold) + std::string(gap, ' ') +
                  paint(status, m.paused ? k_yellow : k_dim));
    out.push_back(std::string(width, ' '));

    for (std::string& line : trade_lines(rec, m, width, paint)) {
        out.push_back(std::move(line));
    }
    out.push_back(std::string(width, ' '));

    const std::vector<NodeId> nodes = involved_nodes(rec);
    if (nodes.size() == 4 && width < 104) {
        // Two rows of two panels.
        for (const auto& pair : {std::vector<NodeId>{nodes[0], nodes[1]},
                                 std::vector<NodeId>{nodes[2], nodes[3]}}) {
            for (std::string& line : panel_rows(rec, m, pair, width, paint)) {
                out.push_back(std::move(line));
            }
        }
    } else {
        for (std::string& line : panel_rows(rec, m, nodes, width, paint)) {
            out.push_back(std::move(line));
        }
    }
    out.push_back(std::string(width, ' '));
    for (std::string& line : wire_lines(rec, m, width, paint)) {
        out.push_back(std::move(line));
    }
    out.push_back(std::string(width, ' '));
    for (std::string& line : story_lines(rec, m, width, paint)) {
        out.push_back(std::move(line));
    }
    out.push_back(std::string(width, ' '));
    out.push_back(check_line(rec, m, width, paint));
    return out;
}

} // namespace glassbox::cli
