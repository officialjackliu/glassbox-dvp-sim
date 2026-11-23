// Glass-box player: pacing (how long things stay on screen) and key mapping.

#include "cli/pacing.hpp"
#include "cli/terminal.hpp"
#include "check.hpp"

#include <limits>

namespace {

using glassbox::cli::Key;
using glassbox::cli::Pace;
using glassbox::cli::pace;

[[nodiscard]] bool same(const Pace& p,
                        std::int64_t travel,
                        std::int64_t dwell,
                        glassbox::SimTimeUs skipped)
{
    return p.travel_ms == travel && p.dwell_ms == dwell &&
           p.skipped_us == skipped;
}

void test_slow_motion_rate()
{
    // 1 wall ms shows 25 us at 100%: a 35 ms hop takes 1.4 s.
    CHECK(same(pace(35'000, true, false, 100), 1'400, 1'800, 0));
    CHECK(same(pace(1'000, true, false, 100), 40, 1'800, 0));
    CHECK(same(pace(120'000, true, false, 100), 4'800, 1'800, 0));
    CHECK(same(pace(24, true, false, 100), 0, 1'800, 0)); // below one ms
    CHECK(same(pace(25, true, false, 100), 1, 1'800, 0));
    CHECK(same(pace(0, true, false, 100), 0, 1'800, 0));
    CHECK(same(pace(-5, true, false, 100), 0, 1'800, 0));
    CHECK(same(pace(-100'000, true, false, 100), 0, 1'800, 0));
    CHECK(same(pace(std::numeric_limits<glassbox::SimTimeUs>::min(), false,
                    false, 100),
               0, 1'800, 0));
}

void test_caps_compress_long_gaps()
{
    // Messages on the wire: at most 5 s (125 ms of sim time) shown.
    CHECK(same(pace(124'999, true, false, 100), 4'999, 1'800, 0));
    CHECK(same(pace(125'000, true, false, 100), 5'000, 1'800, 0));
    CHECK(same(pace(125'001, true, false, 100), 5'000, 1'800, 1));
    CHECK(same(pace(300'000, true, false, 100), 5'000, 1'800, 175'000));
    // Nothing on the wire: at most 1.5 s (37.5 ms) shown.
    CHECK(same(pace(37'499, false, false, 100), 1'499, 1'800, 0));
    CHECK(same(pace(37'500, false, false, 100), 1'500, 1'800, 0));
    CHECK(same(pace(37'501, false, false, 100), 1'500, 1'800, 1));
    CHECK(same(pace(355'700, false, false, 100), 1'500, 1'800, 318'200));
    const auto max = std::numeric_limits<glassbox::SimTimeUs>::max();
    CHECK(same(pace(max, true, false, 100), 5'000, 1'800, max - 125'000));
    CHECK(same(pace(max, false, false, 100), 1'500, 1'800, max - 37'500));
}

void test_minor_events_and_speed()
{
    CHECK(same(pace(0, false, true, 100), 0, 600, 0));
    CHECK(same(pace(35'000, true, true, 100), 1'400, 600, 0));
    CHECK(same(pace(35'000, true, false, 200), 700, 900, 0));
    CHECK(same(pace(35'000, true, false, 50), 2'800, 3'600, 0));
    CHECK(same(pace(35'000, true, false, 25), 5'600, 7'200, 0));
    CHECK(same(pace(35'000, true, false, 800), 175, 225, 0));
    CHECK(same(pace(35'000, true, true, 800), 175, 75, 0));
    // Speed is clamped to [25, 800].
    CHECK(same(pace(35'000, true, false, 24), 5'600, 7'200, 0));
    CHECK(same(pace(35'000, true, false, 0), 5'600, 7'200, 0));
    CHECK(same(pace(35'000, true, false, -100), 5'600, 7'200, 0));
    CHECK(same(pace(35'000, true, false, 801), 175, 225, 0));
    CHECK(same(pace(35'000, true, false, 30), 4'666, 6'000, 0)); // in between
}

void test_speed_steps()
{
    using glassbox::cli::faster;
    using glassbox::cli::slower;
    CHECK(faster(25) == 50);
    CHECK(faster(50) == 100);
    CHECK(faster(100) == 200);
    CHECK(faster(200) == 400);
    CHECK(faster(400) == 800);
    CHECK(faster(800) == 800);
    CHECK(faster(799) == 800);
    CHECK(faster(30) == 50);
    CHECK(faster(1'000) == 800);
    CHECK(faster(10) == 25);
    CHECK(slower(800) == 400);
    CHECK(slower(400) == 200);
    CHECK(slower(200) == 100);
    CHECK(slower(100) == 50);
    CHECK(slower(50) == 25);
    CHECK(slower(25) == 25);
    CHECK(slower(26) == 25);
    CHECK(slower(30) == 25);
    CHECK(slower(1'000) == 800);
    CHECK(slower(10) == 25);
}

void test_keys()
{
    using glassbox::cli::key_for;
    CHECK(key_for("") == Key::none);
    CHECK(key_for(" ") == Key::pause);
    CHECK(key_for("p") == Key::pause);
    CHECK(key_for("P") == Key::pause);
    CHECK(key_for("n") == Key::next);
    CHECK(key_for("N") == Key::next);
    CHECK(key_for("\r") == Key::next);
    CHECK(key_for("\n") == Key::next);
    CHECK(key_for("\x1b[C") == Key::next);   // right arrow
    CHECK(key_for("+") == Key::faster);
    CHECK(key_for("=") == Key::faster);
    CHECK(key_for("\x1b[A") == Key::faster); // up arrow
    CHECK(key_for("-") == Key::slower);
    CHECK(key_for("_") == Key::slower);
    CHECK(key_for("\x1b[B") == Key::slower); // down arrow
    CHECK(key_for("r") == Key::restart);
    CHECK(key_for("R") == Key::restart);
    CHECK(key_for("q") == Key::quit);
    CHECK(key_for("Q") == Key::quit);
    CHECK(key_for("\x03") == Key::quit);     // Ctrl-C
    CHECK(key_for("\x1b") == Key::quit);     // Esc alone
    CHECK(key_for("\x1b[D") == Key::none);   // left arrow: unused
    CHECK(key_for("x") == Key::none);
    CHECK(key_for("q extra") == Key::quit);  // first byte decides
}

void test_key_splitting()
{
    using glassbox::cli::first_key_length;
    CHECK(first_key_length("") == 0);
    CHECK(first_key_length("q") == 1);
    CHECK(first_key_length(" +") == 1);
    CHECK(first_key_length("\x1b[A+") == 3);
    CHECK(first_key_length("\x1b[") == 1);  // too short for an arrow
    CHECK(first_key_length("\x1bq") == 1);
    CHECK(first_key_length("\x1b") == 1);
}

} // namespace

int main()
{
    test_slow_motion_rate();
    test_caps_compress_long_gaps();
    test_minor_events_and_speed();
    test_speed_steps();
    test_keys();
    test_key_splitting();
    return 0;
}
