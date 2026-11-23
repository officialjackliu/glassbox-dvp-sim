// Glass-box player: the screen shows exactly what the recording holds, and
// fits the terminal at every width.

#include "cli/screen.hpp"
#include "scenarios/scenario_library.hpp"
#include "check.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using glassbox::NodeId;
using glassbox::cli::Moment;
using glassbox::cli::Recording;
using glassbox::cli::ScreenOptions;
using glassbox::cli::visible_width;
using Lines = std::vector<std::string>;

[[nodiscard]] std::string rstrip(std::string s)
{
    while (!s.empty() && s.back() == ' ') {
        s.pop_back();
    }
    return s;
}

[[nodiscard]] Lines plain(const Recording& rec, Moment m, int width = 100)
{
    Lines out;
    for (const std::string& line :
         glassbox::cli::draw(rec, m, ScreenOptions{width, false})) {
        out.push_back(rstrip(line));
    }
    return out;
}

[[nodiscard]] std::string strip_escapes(const std::string& s)
{
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\x1b') {
            while (i < s.size() && s[i] != 'm') {
                ++i;
            }
            continue;
        }
        out += s[i];
    }
    return out;
}

[[nodiscard]] Recording rec_of(std::string_view id)
{
    return glassbox::cli::record(glassbox::find_scenario(id)->make(1));
}

[[nodiscard]] bool has_line(const Lines& lines, const std::string& wanted)
{
    for (const std::string& l : lines) {
        if (l == wanted) {
            return true;
        }
    }
    return false;
}

void test_visible_width()
{
    CHECK(visible_width("") == 0);
    CHECK(visible_width("abc") == 3);
    CHECK(visible_width("✓") == 1);
    CHECK(visible_width("─┌┐│") == 4);
    CHECK(visible_width("µs") == 2);
    CHECK(visible_width("…") == 1);
    CHECK(visible_width("\x1b[32m✓ ok\x1b[0m") == 4);
    CHECK(visible_width("\x1b[1;7mX\x1b[0m") == 1);
}

void test_s1_screen_exact()
{
    const Recording rec = rec_of("S1");
    const Lines expected{
        "t = 3.600 ms                                                     ▶ "
        "playing · speed 100% · event 7/28",
        "",
        "#1  BANK_A buys 25 UST 10Y bonds from BANK_B for $24,875.00",
        "    ✓ submit ─ ✓ lock ─ ● vote ─ ○ decide ─ ○ settle ─ ○ confirm",
        "",
        "┌─ COORD · New York ───────────┐ ┌─ BANK_A · New York ──────────┐ "
        "┌─ BANK_B · London ────────────┐",
        "│ #1 COLLECTING                │ │ cash             $975,125.00 │ "
        "│ cash           $1,000,000.00 │",
        "│   votes A ·  B ·             │ │   locked          $24,875.00 │ "
        "│ bonds                 25,000 │",
        "│ timer vote @501.100 ms       │ │ bonds                 25,000 │ "
        "│ disk  (empty)                │",
        "│ disk                         │ │ #1 PREPARED (voted YES)      │ "
        "│                              │",
        "│   PROPOSED t1                │ │ timer ask @1,003.200 ms      │ "
        "│                              │",
        "│                              │ │ disk                         │ "
        "│                              │",
        "│                              │ │   PREPARED t1                │ "
        "│                              │",
        "└──────────────────────────────┘ └──────────────────────────────┘ "
        "└──────────────────────────────┘",
        "",
        "On the wire   · next event in 600 µs",
        "  PREPARE     COORD ──●───────────────────────────────▶ BANK_B   "
        "35.0 ms  New York → London",
        "  VOTE YES   BANK_A ─────────────●────────────────────▶ COORD     "
        "1.0 ms  within New York",
        "",
        "▶ BANK_A sends its YES vote to COORD",
        "   · its PREPARED record is already on disk, so the promise survives "
        "a crash",
        "   · travels 1.0 ms, within New York",
        "   code Effects::release ← Participant::on_prepare",
        "",
        "✓ money conserved   ✓ no negative balances   ✓ locks match open trades",
    };
    const Lines got = plain(rec, Moment{7, 3'600, 100, false});
    if (got != expected) {
        for (const std::string& l : got) {
            std::fprintf(stderr, "%s|\n", l.c_str());
        }
    }
    CHECK(got == expected);

    // At the event itself the dot sits at the start and nothing is "next".
    const Lines at_event = plain(rec, Moment{7, 3'200, 100, false});
    CHECK(at_event[0].starts_with("t = 3.200 ms "));
    CHECK(at_event[15] == "On the wire");
    CHECK(at_event[17] ==
          "  VOTE YES   BANK_A ●─────────────────────────────────▶ COORD     "
          "1.0 ms  within New York");
}

void test_moment_is_clamped_between_events()
{
    const Recording rec = rec_of("S1");
    // Before the event: shown at the event. After the next: at the next.
    CHECK(plain(rec, Moment{7, 0, 100, false})[0].starts_with("t = 3.200 ms "));
    CHECK(plain(rec, Moment{7, 4'199, 100, false})[0].starts_with(
        "t = 4.199 ms "));
    CHECK(plain(rec, Moment{7, 99'999'999, 100, false})[0].starts_with(
        "t = 4.200 ms "));
    // A frame past the end shows the last event.
    CHECK(plain(rec, Moment{999, 0, 100, false})[0].ends_with("event 28/28"));
    // Quiet gap: the next event is announced.
    CHECK(has_line(plain(rec, Moment{24, 200'000, 100, false}),
                   "On the wire   · next event in 301.1 ms"));
    // Status: paused and speed.
    CHECK(plain(rec, Moment{3, 0, 50, true})[0].ends_with(
        "❚❚ paused · speed 50% · event 3/28"));
}

void test_endings()
{
    const Lines commit = plain(rec_of("S1"), Moment{28, 0, 100, false});
    CHECK(commit[3] == "    ✓ submit ─ ✓ lock ─ ✓ vote ─ ✓ decide ─ ✓ settle "
                       "─ ✓ confirm   SETTLED: both legs moved");
    CHECK(has_line(commit, "│ #1 DONE (COMMIT)             │ │ cash             "
                           "$975,125.00 │ │ cash           $1,024,875.00 │"));
    const Lines abort = plain(rec_of("S2"), Moment{999, 0, 100, false});
    CHECK(abort[3] == "    ✓ submit ─ ✓ lock ─ ✓ vote ─ ✓ decide ─ ✓ settle "
                      "─ ✓ confirm   CANCELLED: nothing moved");
    // Narrow terminals get the short outcome.
    CHECK(plain(rec_of("S1"), Moment{28, 0, 100, false}, 80)[3] ==
          "    ✓ submit ─ ✓ lock ─ ✓ vote ─ ✓ decide ─ ✓ settle ─ ✓ confirm   "
          "SETTLED");
    CHECK(plain(rec_of("S2"), Moment{28, 0, 100, false}, 80)[3] ==
          "    ✓ submit ─ ✓ lock ─ ✓ vote ─ ✓ decide ─ ✓ settle ─ ✓ confirm   "
          "CANCELLED");
    // Before the first event there is no trade yet.
    CHECK(plain(rec_of("S1"), Moment{0, 0, 100, false})[2] ==
          "(no trade submitted yet)");
}

void test_down_panels()
{
    // S6 right after BANK_A crashes: memory lost, disk kept, no timers.
    const Lines crashed = plain(rec_of("S6"), Moment{8, 3'200, 100, false});
    CHECK(crashed[5] == "┌─ COORD · New York ───────────┐ ┌─ BANK_A · New York "
                        "──────────┐ ┌─ BANK_B · London ────────────┐");
    CHECK(has_line(crashed,
                   "│   votes A ·  B ·             │ │ disk                 "
                   "        │ │ bonds                 25,000 │"));
    bool down = false;
    for (const std::string& l : crashed) {
        down = down || l.find("│ ✗ DOWN: memory lost          │") !=
                           std::string::npos;
        CHECK(l.find("timer ask") == std::string::npos);
    }
    CHECK(down);

    // S5: BANK_B offline (not crashed) keeps its balances on screen.
    const Lines offline = plain(rec_of("S5"), Moment{2, 1'000, 100, false});
    bool saw = false;
    for (const std::string& l : offline) {
        saw = saw || l.find("│ ✗ OFFLINE: gets no messages  │") !=
                         std::string::npos;
    }
    CHECK(saw);
    // Not crashed, so its balances are still in memory.
    CHECK(has_line(offline, "│   votes A ·  B ·             │ │ bonds        "
                            "         25,000 │ │ cash           $1,000,000.00 │"));
}

[[nodiscard]] bool any_contains(const Lines& lines, const std::string& part)
{
    for (const std::string& l : lines) {
        if (l.find(part) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void test_timers_and_disk_details()
{
    const Recording s1 = rec_of("S1");
    // The vote timer is pending until the instant it fires.
    CHECK(any_contains(plain(s1, Moment{24, 501'099, 100, false}),
                       "timer vote @501.100 ms"));
    CHECK(!any_contains(plain(s1, Moment{25, 501'100, 100, false}),
                        "timer vote"));

    // COORD back 1 ms after its crash: the pre-crash vote timer is stale
    // (it will fire as stale_epoch) and is not shown; the new resend
    // timer is.
    glassbox::ScenarioSetup early =
        glassbox::make_s9_coordinator_crash_after_decision(1);
    early.crashes[0].restart_after_us = 1'000;
    const Recording rec = glassbox::cli::record(early);
    std::size_t after = 0;
    for (std::size_t i = 0; i < rec.frames.size(); ++i) {
        if (rec.frames[i].time_us == 74'200) {
            after = i;
        }
    }
    CHECK(after > 0);
    const Lines restarted = plain(rec, Moment{after, 74'200, 100, false});
    CHECK(!any_contains(restarted, "timer vote"));
    CHECK(any_contains(restarted, "timer resend @1,074.300 ms"));
    CHECK(!any_contains(restarted, "DOWN"));

    // After S6's restart BANK_A is no longer shown as down.
    const Recording s6 = rec_of("S6");
    CHECK(!any_contains(plain(s6, Moment{999, 0, 100, false}), "DOWN"));

    // A long disk log shows its last four records.
    const Lines s13 = plain(rec_of("S13"), Moment{999, 0, 100, false}, 140);
    CHECK(any_contains(s13, "│   … 2 earlier"));
    CHECK(any_contains(s13, "│   END t2"));
    CHECK(!any_contains(s13, "│   PROPOSED t1"));
}

void test_layouts()
{
    // Four nodes: 2x2 below 104 columns, one row from 104 up.
    const Recording s13 = rec_of("S13");
    const auto tops = [&](int width) {
        int n = 0;
        for (const std::string& l : plain(s13, Moment{16, 0, 100, false}, width)) {
            n += l.starts_with("┌─") ? 1 : 0;
        }
        return n;
    };
    CHECK(tops(80) == 2);
    CHECK(tops(103) == 2);
    CHECK(tops(104) == 1);
    CHECK(tops(160) == 1);
    // Three nodes: always one row.
    const Recording s1 = rec_of("S1");
    int rows = 0;
    for (const std::string& l : plain(s1, Moment{7, 0, 100, false}, 80)) {
        rows += l.starts_with("┌─") ? 1 : 0;
    }
    CHECK(rows == 1);
    // Width is clamped to [80, 200].
    CHECK(plain(s1, Moment{7, 0, 100, false}, 10) ==
          plain(s1, Moment{7, 0, 100, false}, 80));
    CHECK(plain(s1, Moment{7, 0, 100, false}, 5'000) ==
          plain(s1, Moment{7, 0, 100, false}, 200));
}

void test_wire_overflow()
{
    // Four trades at once: eight PREPAREs on the wire, six shown.
    glassbox::ScenarioSetup setup = glassbox::make_s1_happy_path(1);
    setup.script.push_back(glassbox::submit(
        0, glassbox::standard_trade(NodeId::bank_c, NodeId::bank_a, 0)));
    setup.script.push_back(glassbox::submit(
        0, glassbox::standard_trade(NodeId::bank_b, NodeId::bank_c, 0)));
    setup.script.push_back(glassbox::submit(
        0, glassbox::standard_trade(NodeId::bank_a, NodeId::bank_c, 0)));
    const Recording rec = glassbox::cli::record(setup);
    std::size_t frame = 0;
    for (std::size_t i = 0; i < rec.frames.size(); ++i) {
        if (glassbox::cli::in_flight(rec, rec.frames[i].time_us).size() == 8) {
            frame = i;
            break;
        }
    }
    CHECK(frame > 0);
    const Lines lines = plain(rec, Moment{frame, 0, 100, false}, 140);
    int prepares = 0;
    for (const std::string& l : lines) {
        prepares += l.starts_with("  PREPARE ") ? 1 : 0;
    }
    CHECK(prepares == 6);
    CHECK(has_line(lines, "  … and 2 more"));
}

void test_every_frame_fits_and_color_only_adds_escapes()
{
    for (const glassbox::ScenarioInfo& info : glassbox::scenario_registry()) {
        const Recording rec = glassbox::cli::record(info.make(1));
        for (std::size_t i = 0; i < rec.frames.size(); ++i) {
            for (const int width : {80, 100, 140, 200}) {
                const Moment m{i, rec.frames[i].time_us + 500, 100, false};
                const Lines mono =
                    glassbox::cli::draw(rec, m, ScreenOptions{width, false});
                const Lines color =
                    glassbox::cli::draw(rec, m, ScreenOptions{width, true});
                CHECK(mono.size() == color.size());
                for (std::size_t l = 0; l < mono.size(); ++l) {
                    CHECK(visible_width(mono[l]) <=
                          static_cast<std::size_t>(width));
                    CHECK(mono[l].find('\x1b') == std::string::npos);
                    CHECK(strip_escapes(color[l]) == mono[l]);
                }
            }
        }
    }
}

} // namespace

int main()
{
    test_visible_width();
    test_s1_screen_exact();
    test_moment_is_clamped_between_events();
    test_endings();
    test_down_panels();
    test_timers_and_disk_details();
    test_layouts();
    test_wire_overflow();
    test_every_frame_fits_and_color_only_adds_escapes();
    return 0;
}
