// Glass-box player: --set edits a scenario exactly, rejects bad input
// without changing anything, and changes the outcome as expected.

#include "cli/dials.hpp"
#include "cli/recording.hpp"
#include "scenarios/scenario_library.hpp"
#include "check.hpp"

#include <string>
#include <vector>

namespace {

using glassbox::NodeId;
using glassbox::ScenarioSetup;
using glassbox::ScriptSetLink;
using glassbox::cli::apply_dial;
using glassbox::cli::DialStatus;

// Everything a dial may touch, for "unchanged on failure" checks.
struct Fingerprint {
    glassbox::SimTimeUs vote_timeout;
    std::size_t script_size;
    std::vector<std::optional<glassbox::SimTimeUs>> restarts;
    friend bool operator==(const Fingerprint&, const Fingerprint&) = default;
};

[[nodiscard]] Fingerprint print(const ScenarioSetup& s)
{
    Fingerprint f{s.config.timers.vote_timeout_us, s.script.size(), {}};
    for (const auto& c : s.crashes) {
        f.restarts.push_back(c.restart_after_us);
    }
    return f;
}

[[nodiscard]] DialStatus on_s1(std::string_view assignment,
                               glassbox::SimTimeUs* vote_timeout = nullptr)
{
    ScenarioSetup setup = glassbox::make_s1_happy_path(1);
    const Fingerprint before = print(setup);
    const DialStatus status = apply_dial(setup, assignment);
    if (status != DialStatus::ok) {
        CHECK(print(setup) == before); // failure changes nothing
    }
    if (vote_timeout) {
        *vote_timeout = setup.config.timers.vote_timeout_us;
    }
    return status;
}

[[nodiscard]] const ScriptSetLink& link_action(const ScenarioSetup& s,
                                               std::size_t from_end)
{
    const auto& action = s.script[s.script.size() - 1 - from_end];
    CHECK(action.at == 0);
    return std::get<ScriptSetLink>(action.action.action);
}

void test_vote_timeout()
{
    glassbox::SimTimeUs t = 0;
    CHECK(on_s1("vote_timeout=30ms", &t) == DialStatus::ok);
    CHECK(t == 30'000);
    CHECK(on_s1("vote_timeout=500us", &t) == DialStatus::ok);
    CHECK(t == 500);
    CHECK(on_s1("vote_timeout=2s", &t) == DialStatus::ok);
    CHECK(t == 2'000'000);
    CHECK(on_s1("vote_timeout=1us", &t) == DialStatus::ok);
    CHECK(t == 1);
    CHECK(on_s1("vote_timeout=3600s", &t) == DialStatus::ok);
    CHECK(t == 3'600'000'000);
    // Out of range: below 1 us, above an hour, negative, overflowing.
    CHECK(on_s1("vote_timeout=0ms") == DialStatus::out_of_range);
    CHECK(on_s1("vote_timeout=0us") == DialStatus::out_of_range);
    CHECK(on_s1("vote_timeout=3600000001us") == DialStatus::out_of_range);
    CHECK(on_s1("vote_timeout=3601s") == DialStatus::out_of_range);
    CHECK(on_s1("vote_timeout=-5ms") == DialStatus::out_of_range);
    CHECK(on_s1("vote_timeout=9223372036854775807s") ==
          DialStatus::out_of_range);
    // Bad values: no unit, no number, fractions, spaces, empty.
    CHECK(on_s1("vote_timeout=30") == DialStatus::bad_value);
    CHECK(on_s1("vote_timeout=ms") == DialStatus::bad_value);
    CHECK(on_s1("vote_timeout=3.5ms") == DialStatus::bad_value);
    CHECK(on_s1("vote_timeout=30 ms") == DialStatus::bad_value);
    CHECK(on_s1("vote_timeout=") == DialStatus::bad_value);
    CHECK(on_s1("vote_timeout=99999999999999999999ms") == DialStatus::bad_value);
}

void test_keys_and_syntax()
{
    CHECK(on_s1("") == DialStatus::bad_value);
    CHECK(on_s1("vote_timeout") == DialStatus::bad_value);
    CHECK(on_s1("bogus=1ms") == DialStatus::unknown_key);
    CHECK(on_s1("Vote_timeout=1ms") == DialStatus::unknown_key);
    CHECK(on_s1("=1ms") == DialStatus::unknown_key);
    CHECK(on_s1("latency.COORD-COORD=1ms") == DialStatus::bad_value);
    CHECK(on_s1("latency.COORD=1ms") == DialStatus::bad_value);
    CHECK(on_s1("latency.COORD-BANK_D=1ms") == DialStatus::bad_value);
    CHECK(on_s1("latency.coord-bank_b=1ms") == DialStatus::bad_value);
    CHECK(on_s1("latency.=1ms") == DialStatus::bad_value);
    CHECK(on_s1("latency.COORD-BANK_B=0ms") == DialStatus::out_of_range);
    CHECK(on_s1("drop.COORD-BANK_B=1ms") == DialStatus::bad_value);
    CHECK(on_s1("latency.COORD-BANK_B=20%") == DialStatus::bad_value);

    CHECK(glassbox::cli::to_string(DialStatus::ok) == "ok");
    CHECK(glassbox::cli::to_string(DialStatus::unknown_key) ==
          "unknown setting (or nothing to apply it to in this scenario)");
    CHECK(glassbox::cli::to_string(DialStatus::bad_value) == "bad value");
    CHECK(glassbox::cli::to_string(DialStatus::out_of_range) ==
          "value out of range");
}

void test_latency_and_drop()
{
    ScenarioSetup s1 = glassbox::make_s1_happy_path(1);
    const std::size_t before = s1.script.size();
    CHECK(apply_dial(s1, "latency.COORD-BANK_B=80ms") == DialStatus::ok);
    CHECK(s1.script.size() == before + 2);
    const ScriptSetLink& forward = link_action(s1, 1);
    const ScriptSetLink& back = link_action(s1, 0);
    CHECK(forward.from == NodeId::coordinator);
    CHECK(forward.to == NodeId::bank_b);
    CHECK(forward.base_latency_us == 80'000);
    CHECK(forward.jitter_us == 0);
    CHECK(forward.drop_probability_ppm == 0);
    CHECK(back.from == NodeId::bank_b);
    CHECK(back.to == NodeId::coordinator);
    CHECK(back.base_latency_us == 80'000);
    // Either order of the pair names the same link.
    ScenarioSetup other = glassbox::make_s1_happy_path(1);
    CHECK(apply_dial(other, "latency.BANK_B-COORD=80ms") == DialStatus::ok);
    CHECK(link_action(other, 1).from == NodeId::bank_b);
    CHECK(link_action(other, 0).from == NodeId::coordinator);

    // The run uses it: PREPARE to London now takes 80 ms.
    const auto rec = glassbox::cli::record(s1);
    bool found = false;
    for (const auto& f : rec.flights) {
        if (f.type == glassbox::MessageType::prepare && f.to == NodeId::bank_b) {
            CHECK(f.sent_at_us == 1'100);
            CHECK(f.ends_at_us == 81'100);
            found = true;
        }
    }
    CHECK(found);

    // Drop: percent to parts per million, keeping latency.
    ScenarioSetup d = glassbox::make_s1_happy_path(1);
    CHECK(apply_dial(d, "drop.COORD-BANK_A=20%") == DialStatus::ok);
    CHECK(link_action(d, 1).drop_probability_ppm == 200'000);
    CHECK(link_action(d, 1).base_latency_us == 1'000);
    CHECK(apply_dial(d, "drop.COORD-BANK_A=0%") == DialStatus::ok);
    CHECK(link_action(d, 1).drop_probability_ppm == 0);
    CHECK(apply_dial(d, "drop.COORD-BANK_A=100%") == DialStatus::ok);
    CHECK(link_action(d, 0).drop_probability_ppm == 1'000'000);
    const Fingerprint fp = print(d);
    CHECK(apply_dial(d, "drop.COORD-BANK_A=101%") == DialStatus::out_of_range);
    CHECK(apply_dial(d, "drop.COORD-BANK_A=-1%") == DialStatus::out_of_range);
    CHECK(apply_dial(d, "drop.COORD-BANK_A=20") == DialStatus::bad_value);
    CHECK(apply_dial(d, "drop.COORD-BANK_A=x%") == DialStatus::bad_value);
    CHECK(apply_dial(d, "drop.COORD-BANK_A=%") == DialStatus::bad_value);
    CHECK(print(d) == fp);

    // Settings stack: a later latency keeps an earlier drop, and the
    // scenario's own jitter is kept (S12: COORD<->BANK_A 20 ms +/- 19 ms).
    ScenarioSetup s12 = glassbox::make_s12_duplicates_and_reordering(1);
    CHECK(apply_dial(s12, "drop.COORD-BANK_A=10%") == DialStatus::ok);
    CHECK(apply_dial(s12, "latency.COORD-BANK_A=50ms") == DialStatus::ok);
    CHECK(link_action(s12, 1).base_latency_us == 50'000);
    CHECK(link_action(s12, 1).jitter_us == 19'000);
    CHECK(link_action(s12, 1).drop_probability_ppm == 100'000);
    CHECK(link_action(s12, 0).jitter_us == 19'000);
}

void test_each_direction_keeps_its_own_start_state()
{
    // COORD -> BANK_B has jitter at t = 0, BANK_B -> COORD does not, and a
    // later change at 0.5 s must not leak into the start state.
    ScenarioSetup s = glassbox::make_s1_happy_path(1);
    s.script.push_back(glassbox::TimedAction{
        0, glassbox::ScriptActionEvent{
               ScriptSetLink{NodeId::coordinator, NodeId::bank_b, 35'000, 7'000, 0}}});
    s.script.push_back(glassbox::TimedAction{
        500'000, glassbox::ScriptActionEvent{ScriptSetLink{
                     NodeId::bank_b, NodeId::coordinator, 99'000, 5'000, 300'000}}});
    CHECK(apply_dial(s, "latency.COORD-BANK_B=80ms") == DialStatus::ok);
    const ScriptSetLink& forward = link_action(s, 1);
    const ScriptSetLink& back = link_action(s, 0);
    CHECK(forward.base_latency_us == 80'000);
    CHECK(forward.jitter_us == 7'000);
    CHECK(forward.drop_probability_ppm == 0);
    CHECK(back.base_latency_us == 80'000);
    CHECK(back.jitter_us == 0);
    CHECK(back.drop_probability_ppm == 0);
}

void test_restart_after()
{
    ScenarioSetup s6 = glassbox::make_s6_in_doubt_participant(1);
    CHECK(apply_dial(s6, "restart_after=2s") == DialStatus::ok);
    CHECK(s6.crashes[0].restart_after_us == 2'000'000);
    const Fingerprint fp = print(s6);
    CHECK(apply_dial(s6, "restart_after=0s") == DialStatus::out_of_range);
    CHECK(apply_dial(s6, "restart_after=2") == DialStatus::bad_value);
    CHECK(print(s6) == fp);
    // No crash point in S1: nothing to apply it to.
    CHECK(on_s1("restart_after=2s") == DialStatus::unknown_key);
}

void test_settings_change_outcomes()
{
    // A 30 ms vote timeout aborts the happy path: the PREPARE needs 35 ms
    // just to reach London.
    ScenarioSetup tight = glassbox::make_s1_happy_path(1);
    CHECK(apply_dial(tight, "vote_timeout=30ms") == DialStatus::ok);
    const auto aborted = glassbox::cli::record(tight);
    CHECK(aborted.quiesced);
    const auto& coord = aborted.frames.back().nodes[0].coordinator_trades;
    CHECK(coord.at(glassbox::TradeId{1}).decision == glassbox::Decision::abort);
    CHECK(coord.at(glassbox::TradeId{1}).abort_reason ==
          glassbox::ReasonCode::vote_timeout);

    // A 900 ms vote timeout cures S4's false abort.
    ScenarioSetup patient = glassbox::make_s4_slow_link_tight_timeout(1);
    CHECK(apply_dial(patient, "vote_timeout=900ms") == DialStatus::ok);
    const auto committed = glassbox::cli::record(patient);
    CHECK(committed.frames.back()
              .nodes[0]
              .coordinator_trades.at(glassbox::TradeId{1})
              .decision == glassbox::Decision::commit);
    for (const auto& frame : committed.frames) {
        CHECK(frame.violations.empty());
    }
}

} // namespace

int main()
{
    test_vote_timeout();
    test_keys_and_syntax();
    test_latency_and_drop();
    test_each_direction_keeps_its_own_start_state();
    test_restart_after();
    test_settings_change_outcomes();
    return 0;
}
