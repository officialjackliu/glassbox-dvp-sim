// Glass-box player: the narrator explains each recorded event exactly, with
// amounts, checks, reasons and the function that ran.

#include "cli/narrator.hpp"
#include "scenarios/scenario_library.hpp"
#include "check.hpp"

#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

using glassbox::Cents;
using glassbox::NodeId;
using glassbox::cli::Recording;
using glassbox::cli::Stage;
using glassbox::cli::Story;
using glassbox::cli::Tone;
using Lines = std::vector<std::string>;

struct Expected {
    std::string headline;
    Lines details;
    std::string why;
    std::string code;
    std::optional<Stage> stage;
    Tone tone;
};

void check_story(const Recording& rec, std::size_t frame, const Expected& e)
{
    const Story s = glassbox::cli::narrate(rec, frame);
    if (s.headline != e.headline || s.details != e.details || s.why != e.why ||
        s.code != e.code || s.stage != e.stage || s.tone != e.tone) {
        std::fprintf(stderr, "frame %zu: got \"%s\"\n", frame,
                     s.headline.c_str());
        for (const std::string& d : s.details) {
            std::fprintf(stderr, "    %s\n", d.c_str());
        }
        std::fprintf(stderr, "  why \"%s\" code \"%s\"\n", s.why.c_str(),
                     s.code.c_str());
    }
    CHECK(s.headline == e.headline);
    CHECK(s.details == e.details);
    CHECK(s.why == e.why);
    CHECK(s.code == e.code);
    CHECK(s.stage == e.stage);
    CHECK(s.tone == e.tone);
}

[[nodiscard]] Recording rec_of(std::string_view id)
{
    return glassbox::cli::record(glassbox::find_scenario(id)->make(1));
}

// --- Formatting -------------------------------------------------------------

void test_formatting()
{
    using glassbox::cli::dollars;
    CHECK(dollars(Cents{0}) == "$0.00");
    CHECK(dollars(Cents{1}) == "$0.01");
    CHECK(dollars(Cents{9}) == "$0.09");
    CHECK(dollars(Cents{10}) == "$0.10");
    CHECK(dollars(Cents{99}) == "$0.99");
    CHECK(dollars(Cents{100}) == "$1.00");
    CHECK(dollars(Cents{99'999}) == "$999.99");
    CHECK(dollars(Cents{100'000}) == "$1,000.00");
    CHECK(dollars(Cents{2'487'500}) == "$24,875.00");
    CHECK(dollars(Cents{2'487'499}) == "$24,874.99");
    CHECK(dollars(Cents{100'000'000}) == "$1,000,000.00");
    CHECK(dollars(Cents{-1}) == "-$0.01");
    CHECK(dollars(Cents{-2'487'500}) == "-$24,875.00");
    CHECK(dollars(Cents{std::numeric_limits<std::int64_t>::max()}) ==
          "$92,233,720,368,547,758.07");
    CHECK(dollars(Cents{std::numeric_limits<std::int64_t>::min()}) ==
          "-$92,233,720,368,547,758.08");

    using glassbox::cli::with_commas;
    CHECK(with_commas(0) == "0");
    CHECK(with_commas(999) == "999");
    CHECK(with_commas(1'000) == "1,000");
    CHECK(with_commas(999'999) == "999,999");
    CHECK(with_commas(1'000'000) == "1,000,000");
    CHECK(with_commas(-1'000) == "-1,000");
    CHECK(with_commas(-999) == "-999");
    CHECK(with_commas(std::numeric_limits<std::int64_t>::min()) ==
          "-9,223,372,036,854,775,808");

    using glassbox::cli::format_time;
    CHECK(format_time(0) == "0.000 ms");
    CHECK(format_time(1) == "0.001 ms");
    CHECK(format_time(999) == "0.999 ms");
    CHECK(format_time(1'000) == "1.000 ms");
    CHECK(format_time(3'200) == "3.200 ms");
    CHECK(format_time(999'999) == "999.999 ms");
    CHECK(format_time(1'000'000) == "1,000.000 ms");
    CHECK(format_time(1'074'400) == "1,074.400 ms");
    CHECK(format_time(-1'500) == "-1.500 ms");

    using glassbox::cli::format_duration;
    CHECK(format_duration(0) == "0 µs");
    CHECK(format_duration(100) == "100 µs");
    CHECK(format_duration(999) == "999 µs");
    CHECK(format_duration(1'000) == "1.0 ms");
    CHECK(format_duration(1'099) == "1.0 ms");
    CHECK(format_duration(1'100) == "1.1 ms");
    CHECK(format_duration(35'000) == "35.0 ms");
    CHECK(format_duration(999'999) == "999.9 ms");
    CHECK(format_duration(1'000'000) == "1.000 s");
    CHECK(format_duration(1'500'000) == "1.500 s");
    CHECK(format_duration(5'000'000) == "5.000 s");
    CHECK(format_duration(12'345'678'000) == "12,345.678 s");
    CHECK(format_duration(-1'000) == "-1.0 ms");
    CHECK(format_duration(std::numeric_limits<std::int64_t>::min()) ==
          "-9,223,372,036,854.775 s");

    CHECK(glassbox::cli::city(NodeId::coordinator) == "New York");
    CHECK(glassbox::cli::city(NodeId::bank_a) == "New York");
    CHECK(glassbox::cli::city(NodeId::bank_b) == "London");
    CHECK(glassbox::cli::city(NodeId::bank_c) == "Singapore");

    CHECK(glassbox::cli::bonds_text(0) == "0 UST 10Y bonds");
    CHECK(glassbox::cli::bonds_text(1) == "1 UST 10Y bond");
    CHECK(glassbox::cli::bonds_text(2) == "2 UST 10Y bonds");
    CHECK(glassbox::cli::bonds_text(25'000) == "25,000 UST 10Y bonds");

    CHECK(glassbox::cli::to_string(Stage::submit) == "submit");
    CHECK(glassbox::cli::to_string(Stage::lock) == "lock");
    CHECK(glassbox::cli::to_string(Stage::vote) == "vote");
    CHECK(glassbox::cli::to_string(Stage::decide) == "decide");
    CHECK(glassbox::cli::to_string(Stage::settle) == "settle");
    CHECK(glassbox::cli::to_string(Stage::confirm) == "confirm");
}

// --- S1: the whole happy path ---------------------------------------------

void test_s1_story()
{
    const Recording rec = rec_of("S1");
    check_story(rec, 0, {"Ready: nothing has happened yet", {}, "", "",
                         std::nullopt, Tone::info});
    check_story(rec, rec.frames.size(),
                {"End of the recording", {}, "", "", std::nullopt, Tone::info});
    check_story(
        rec, 1,
        {"Trade #1 submitted: BANK_A buys 25 UST 10Y bonds from BANK_B for "
         "$24,875.00",
         {"COORD checks the ticket: two different known banks, asset, "
          "quantity, price, deadline ✓",
          "COORD opens trade #1 and starts collecting votes (vote timer: "
          "501.100 ms)",
          "next: write PROPOSED to disk, then ask both banks to PREPARE"},
         "Delivery versus payment: the bonds move one way and the cash the "
         "other, or nothing moves at all.",
         "Coordinator::on_propose", Stage::submit, Tone::info});
    check_story(rec, 2,
                {"COORD writes PROPOSED to disk",
                 {"trade #1 now survives a crash and its id is never reused",
                  "next: PREPARE to BANK_A (buyer) and BANK_B (seller)"},
                 "Write-ahead rule: put it on disk first, then act on it.",
                 "Effects::flush", Stage::submit, Tone::info});
    check_story(rec, 4,
                {"COORD sends PREPARE to BANK_B (seller leg)",
                 {"asks BANK_B to lock 25 UST 10Y bonds and vote",
                  "travels 35.0 ms, New York → London"},
                 "", "Effects::release ← Coordinator::on_propose", Stage::lock,
                 Tone::info});
    check_story(
        rec, 5,
        {"BANK_A checks the trade and locks $24,875.00 cash",
         {"account not frozen ✓",
          "deadline not passed ✓ (valid until 30,000.000 ms)",
          "free cash $1,000,000.00 ≥ $24,875.00 ✓",
          "locks $24,875.00 → free cash now $975,125.00",
          "will vote YES as soon as its PREPARED record is on disk"},
         "Locking first means the same cash can't be promised to two trades.",
         "Participant::on_prepare", Stage::lock, Tone::info});
    check_story(rec, 6,
                {"BANK_A's promise is on disk (PREPARED)",
                 {"from now on BANK_A may not back out on its own",
                  "after a crash it restores the lock from disk and asks for "
                  "the outcome"},
                 "A bank votes YES only after its promise is on disk.",
                 "Effects::flush", Stage::lock, Tone::info});
    check_story(rec, 7,
                {"BANK_A sends its YES vote to COORD",
                 {"its PREPARED record is already on disk, so the promise "
                  "survives a crash",
                  "travels 1.0 ms, within New York"},
                 "", "Effects::release ← Participant::on_prepare", Stage::vote,
                 Tone::info});
    check_story(rec, 8,
                {"COORD records BANK_A's YES (1 of 2)",
                 {"still waiting for BANK_B"}, "", "Coordinator::on_vote",
                 Stage::vote, Tone::info});
    check_story(
        rec, 9,
        {"BANK_B checks the trade and locks 25 UST 10Y bonds",
         {"account not frozen ✓",
          "deadline not passed ✓ (valid until 30,000.000 ms)",
          "free bonds 25,000 ≥ 25 ✓",
          "locks 25 UST 10Y bonds → free bonds now 24,975",
          "will vote YES as soon as its PREPARED record is on disk"},
         "Locking first means the same bonds can't be sold twice.",
         "Participant::on_prepare", Stage::lock, Tone::info});
    check_story(rec, 12,
                {"Both banks voted YES → COORD decides COMMIT",
                 {"COORD records BANK_B's YES (2 of 2)",
                  "writes DECISION_COMMIT to disk before telling anyone"},
                 "Once the decision is on disk it never changes, even if "
                 "COORD crashes.",
                 "Coordinator::on_vote → Coordinator::decide", Stage::decide,
                 Tone::good});
    check_story(rec, 13,
                {"COMMIT decision is on disk: the commit point",
                 {"trade #1 will settle now, even if COORD crashes",
                  "next: send COMMIT to both banks"},
                 "The decision is durable before anyone hears it, so it can "
                 "never be taken back.",
                 "Effects::flush", Stage::decide, Tone::good});
    check_story(rec, 14,
                {"COORD sends COMMIT to BANK_A",
                 {"the decision is on disk, so it is safe to announce",
                  "travels 1.0 ms, within New York"},
                 "",
                 "Effects::release ← Coordinator::on_vote → "
                 "Coordinator::decide",
                 Stage::settle, Tone::info});
    check_story(
        rec, 16,
        {"BANK_A settles: pays $24,875.00 and receives 25 UST 10Y bonds",
         {"the locked cash is paid out: cash $1,000,000.00 → $975,125.00",
          "bonds 25,000 → 25,025", "writes COMMITTED to disk, then sends ACK"},
         "Both legs settle on the same decision: this is delivery versus "
         "payment.",
         "Participant::on_commit", Stage::settle, Tone::good});
    check_story(rec, 17,
                {"BANK_A's settlement is on disk (COMMITTED)",
                 {"next: send ACK to COORD"}, "", "Effects::flush",
                 Stage::settle, Tone::good});
    check_story(rec, 18,
                {"BANK_A sends ACK (COMMIT) to COORD",
                 {"lets COORD stop resending to BANK_A",
                  "travels 1.0 ms, within New York"},
                 "", "Effects::release ← Participant::on_commit",
                 Stage::confirm, Tone::info});
    check_story(rec, 19,
                {"COORD notes BANK_A's ACK", {"waiting for BANK_B to confirm"},
                 "", "Coordinator::on_ack", Stage::confirm, Tone::info});
    check_story(
        rec, 20,
        {"BANK_B settles: delivers 25 UST 10Y bonds and receives $24,875.00",
         {"the locked bonds are delivered: bonds 25,000 → 24,975",
          "cash $1,000,000.00 → $1,024,875.00",
          "writes COMMITTED to disk, then sends ACK"},
         "Both legs settle on the same decision: this is delivery versus "
         "payment.",
         "Participant::on_commit", Stage::settle, Tone::good});
    check_story(rec, 23,
                {"Both banks confirmed → trade #1 is complete",
                 {"COORD writes END: nothing left to resend"}, "",
                 "Coordinator::on_ack", Stage::confirm, Tone::good});
    check_story(rec, 24,
                {"COORD writes END: trade #1 is finished",
                 {"no more resends; leftover timers will be ignored"}, "",
                 "Effects::flush", Stage::confirm, Tone::good});
    check_story(rec, 26,
                {"BANK_A's decision-wait timer fires and is ignored",
                 {"trade #1 already moved past what this timer was waiting "
                  "for"},
                 "Timers are never cancelled; when one fires, the simulator "
                 "checks whether it still matters.",
                 "Simulator::dispatch_timer", std::nullopt, Tone::info});

    // Only the leftover timer frames are minor.
    for (std::size_t i = 0; i < rec.frames.size(); ++i) {
        CHECK(glassbox::cli::is_minor(rec.frames[i]) == (i >= 25));
    }
}

// --- Refusals ---------------------------------------------------------------

void test_refusals()
{
    const Recording s2 = rec_of("S2");
    check_story(s2, 5,
                {"BANK_A refuses trade #1: not enough free cash",
                 {"account not frozen ✓",
                  "deadline not passed ✓ (valid until 30,000.000 ms)",
                  "free cash $24,874.99 < $24,875.00 ✗",
                  "writes ABORTED to disk and votes NO; nothing is locked"},
                 "One NO is enough to cancel the trade: both legs settle or "
                 "neither does.",
                 "Participant::on_prepare", Stage::vote, Tone::failure});
    // Its own ABORTED record, before COORD decided, is part of voting.
    check_story(s2, 6,
                {"BANK_A writes ABORTED to disk", {"trade #1 is closed for BANK_A"},
                 "", "Effects::flush", Stage::vote, Tone::info});
    check_story(s2, 8,
                {"BANK_A voted NO (not enough free cash) → COORD decides ABORT",
                 {"writes DECISION_ABORT to disk, then tells both banks to "
                  "release their locks"},
                 "", "Coordinator::on_vote → Coordinator::decide",
                 Stage::decide, Tone::failure});

    const Recording s3 = rec_of("S3");
    check_story(s3, 9,
                {"BANK_B refuses trade #1: account frozen",
                 {"account frozen ✗",
                  "writes ABORTED to disk and votes NO; nothing is locked"},
                 "One NO is enough to cancel the trade: both legs settle or "
                 "neither does.",
                 "Participant::on_prepare", Stage::vote, Tone::failure});
    check_story(s3, 16,
                {"BANK_A releases its lock: $24,875.00 cash is free again",
                 {"COORD's reason: account frozen",
                  "writes ABORTED to disk, then sends ACK"},
                 "", "Participant::on_abort", Stage::settle, Tone::warning});

    // Two trades: headlines name the trade; the refused one says so itself.
    const Recording s13 = rec_of("S13");
    const auto headline = [&](std::size_t i) {
        return glassbox::cli::narrate(s13, i).headline;
    };
    CHECK(headline(1) == "Trade #1 submitted: BANK_A buys 25 UST 10Y bonds "
                         "from BANK_B for $24,875.00");
    CHECK(headline(2) == "Trade #2 submitted: BANK_C buys 25 UST 10Y bonds "
                         "from BANK_B for $24,875.00");
    CHECK(headline(3) == "Trade #1 · COORD writes PROPOSED to disk");
    std::optional<std::size_t> refused;
    for (std::size_t i = 1; i < s13.frames.size(); ++i) {
        if (headline(i) == "BANK_B refuses trade #2: not enough free bonds") {
            refused = i;
        }
    }
    CHECK(refused.has_value());
    CHECK(glassbox::cli::narrate(s13, *refused).details ==
          (Lines{"account not frozen ✓",
                 "deadline not passed ✓ (valid until 30,000.000 ms)",
                 "free bonds 15 < 25 ✗",
                 "writes ABORTED to disk and votes NO; nothing is locked"}));
    CHECK(headline(*refused - 1) ==
          "Trade #1 · BANK_B checks the trade and locks 25 UST 10Y bonds");
}

// --- Timeouts, drops, network faults ----------------------------------------

void test_timeouts_and_network()
{
    const Recording s5 = rec_of("S5");
    check_story(s5, 1,
                {"BANK_B goes offline: it receives nothing until it is back",
                 {}, "", "Simulator::dispatch_script", std::nullopt,
                 Tone::failure});
    check_story(s5, 11,
                {"Vote timer runs out → COORD decides ABORT",
                 {"no vote from BANK_B in time",
                  "writes DECISION_ABORT (VOTE_TIMEOUT) to disk, then tells "
                  "both banks"},
                 "Until it decides, COORD may abort on its own: silence "
                 "counts as NO.",
                 "Coordinator::on_vote_timeout → Coordinator::decide",
                 Stage::decide, Tone::failure});
    check_story(s5, 23,
                {"ABORT from COORD to BANK_B is lost",
                 {"BANK_B is down", "COORD resends it until BANK_B confirms"},
                 "", "NetworkModel", std::nullopt, Tone::failure});

    const Recording s4 = rec_of("S4");
    check_story(s4, 1,
                {"Network change: COORD → BANK_C now takes 300.0 ms", {}, "",
                 "NetworkModel::set_link", std::nullopt, Tone::warning});
    check_story(s4, 15,
                {"ABORT decision is on disk",
                 {"reason: a vote did not arrive in time",
                  "next: send ABORT to both banks"},
                 "", "Effects::flush", Stage::decide, Tone::failure});

    const Recording s11 = rec_of("S11");
    check_story(s11, 1,
                {"Fault injected: the next COMMIT from COORD to BANK_A will "
                 "be lost",
                 {}, "", "Simulator::dispatch_script", std::nullopt,
                 Tone::warning});

    const Recording s12 = rec_of("S12");
    check_story(s12, 1,
                {"Network change: COORD → BANK_A now takes 20.0 ms (± 19.0 ms)",
                 {}, "", "NetworkModel::set_link", std::nullopt,
                 Tone::warning});
    int repeats = 0;
    for (std::size_t i = 1; i < s12.frames.size(); ++i) {
        const Story s = glassbox::cli::narrate(s12, i);
        if (s.headline.ends_with("gets PREPARE again and repeats its YES")) {
            ++repeats;
            CHECK(s.details == Lines{"a duplicate never locks twice"});
            CHECK(s.tone == Tone::warning);
        }
    }
    CHECK(repeats == 2);

    const Recording s14 = rec_of("S14");
    check_story(s14, 10,
                {"Network fault: the link COORD ⇄ BANK_C is cut",
                 {"messages sent on this link from now on are lost",
                  "both nodes keep running; they just can't hear each other"},
                 "A partition is not a crash: nobody loses memory, only "
                 "messages.",
                 "NetworkModel::set_partition", std::nullopt, Tone::failure});
    check_story(s14, 30,
                {"The link COORD ⇄ BANK_C is restored",
                 {"messages get through again"}, "",
                 "NetworkModel::set_partition", std::nullopt, Tone::good});
    bool saw_partition_drop = false;
    for (std::size_t i = 1; i < s14.frames.size(); ++i) {
        const Story s = glassbox::cli::narrate(s14, i);
        if (s.headline == "VOTE from BANK_C to COORD is lost") {
            saw_partition_drop = true;
            CHECK(s.details ==
                  (Lines{"the link BANK_C ⇄ COORD is cut",
                         "COORD will not hear BANK_C's vote"}));
        }
    }
    CHECK(saw_partition_drop);
}

// --- Crashes and recovery -----------------------------------------------------

void test_crash_and_recovery()
{
    const Recording s6 = rec_of("S6");
    check_story(s6, 8,
                {"BANK_A CRASHES right after its first message left",
                 {"memory lost: open trades, locks, timers",
                  "disk kept: PREPARED t1",
                  "messages sent to BANK_A are lost until it restarts"},
                 "Its disk write finished and only its first message left.",
                 "Node::on_crash", std::nullopt, Tone::failure});
    std::optional<std::size_t> restart;
    for (std::size_t i = 1; i < s6.frames.size(); ++i) {
        if (glassbox::cli::narrate(s6, i).headline ==
            "BANK_A restarts and rebuilds itself from disk") {
            restart = i;
        }
    }
    CHECK(restart.has_value());
    check_story(s6, *restart,
                {"BANK_A restarts and rebuilds itself from disk",
                 {"replays its disk: PREPARED t1",
                  "trade #1 is IN DOUBT: lock restored ($24,875.00 cash), asks "
                  "COORD for the outcome"},
                 "Everything in memory was lost; the disk is the only truth.",
                 "Participant::on_restart", std::nullopt, Tone::good});
    // The COMMIT that answers BANK_A's question is attributed to the query
    // handler, not to an earlier retransmit.
    check_story(s6, 32,
                {"BANK_A asks for the outcome; COORD answers COMMIT",
                 {"the answer comes from the decision on COORD's disk"}, "",
                 "Coordinator::on_outcome_query", std::nullopt, Tone::good});
    check_story(s6, 33,
                {"COORD sends COMMIT to BANK_A",
                 {"the decision is on disk, so it is safe to announce",
                  "travels 1.0 ms, within New York"},
                 "", "Effects::release ← Coordinator::on_outcome_query",
                 Stage::settle, Tone::info});

    const Recording s8 = rec_of("S8");
    std::optional<std::size_t> clean;
    for (std::size_t i = 1; i < s8.frames.size(); ++i) {
        const Story s = glassbox::cli::narrate(s8, i);
        if (s.headline == "BANK_A CRASHES before writing anything to disk") {
            CHECK(s.details ==
                  (Lines{"it was handling PREPARE from COORD",
                         "memory lost: open trades, locks, timers",
                         "disk kept: nothing",
                         "messages sent to BANK_A are lost until it "
                         "restarts"}));
            CHECK(s.why == "Nothing from this step reached disk or the "
                           "network.");
        }
        if (s.headline == "BANK_A restarts and rebuilds itself from disk") {
            clean = i;
            CHECK(s.details ==
                  (Lines{"replays its disk: nothing",
                         "nothing on disk: it starts clean and remembers no "
                         "trades"}));
        }
        if (s.headline == "BANK_A never saw trade #1; it records ABORTED "
                          "anyway") {
            CHECK(s.details ==
                  (Lines{"so a PREPARE that arrives late will be refused",
                         "then sends ACK"}));
        }
    }
    CHECK(clean.has_value());

    const Recording s9 = rec_of("S9");
    check_story(s9, 14,
                {"COORD CRASHES after writing to disk, before sending",
                 {"memory lost: votes, decisions in progress, timers",
                  "disk kept: PROPOSED t1, DECISION_COMMIT t1",
                  "messages sent to COORD are lost until it restarts"},
                 "Its disk write finished, but none of its messages left.",
                 "Node::on_crash", std::nullopt, Tone::failure});
    check_story(s9, 16,
                {"BANK_A is in doubt and asks COORD for the outcome",
                 {"it voted YES and holds its lock, but has heard nothing",
                  "question #1; the wait doubles each time"},
                 "A prepared bank may not decide alone: this is why two-phase "
                 "commit can block.",
                 "Participant::on_decision_wait", std::nullopt,
                 Tone::warning});
    check_story(s9, 17,
                {"BANK_A asks COORD: what was decided for trade #1?",
                 {"a bank that voted YES may not decide alone; it must ask",
                  "travels 1.0 ms, within New York"},
                 "", "Effects::release ← Participant::on_decision_wait",
                 std::nullopt, Tone::warning});
    check_story(s9, 29,
                {"COORD sends COMMIT to BANK_A",
                 {"the decision is on disk, so it is safe to announce",
                  "travels 1.0 ms, within New York"},
                 "", "Effects::release ← Coordinator::on_restart",
                 Stage::settle, Tone::info});
    bool resend = false;
    for (std::size_t i = 1; i < s9.frames.size(); ++i) {
        const Story s = glassbox::cli::narrate(s9, i);
        if (s.headline == "COORD restarts and rebuilds itself from disk") {
            resend = true;
            CHECK(s.details ==
                  (Lines{"replays its disk: PROPOSED t1, DECISION_COMMIT t1",
                         "trade #1: decision on disk but no END → resends it "
                         "to both banks"}));
            CHECK(s.code == "Coordinator::on_restart");
        }
        if (s.headline == "COORD's vote timer fires and is ignored") {
            CHECK(s.details == Lines{"COORD is down"});
        }
    }
    CHECK(resend);
}

void test_check_boundaries_and_existing_locks()
{
    // Free cash exactly equal to the price passes the check (>=).
    glassbox::ScenarioSetup exact = glassbox::make_s1_happy_path(1);
    exact.genesis.bank_a = glassbox::make_ledger(2'487'500, 25'000);
    const Recording rec = glassbox::cli::record(exact);
    const Story s = glassbox::cli::narrate(rec, 5);
    CHECK(s.headline == "BANK_A checks the trade and locks $24,875.00 cash");
    CHECK(s.details[2] == "free cash $24,875.00 ≥ $24,875.00 ✓");
    CHECK(s.details[3] == "locks $24,875.00 → free cash now $0.00");

    // A buyer in two trades at once: the second check sees only the cash
    // the first trade left free.
    glassbox::ScenarioSetup two = glassbox::make_s1_happy_path(1);
    two.script.push_back(glassbox::submit(
        0, glassbox::standard_trade(NodeId::bank_a, NodeId::bank_c, 0)));
    const Recording both = glassbox::cli::record(two);
    Lines second;
    for (std::size_t i = 1; i < both.frames.size(); ++i) {
        const Story t = glassbox::cli::narrate(both, i);
        if (t.headline ==
            "Trade #2 · BANK_A checks the trade and locks $24,875.00 cash") {
            second = t.details;
        }
    }
    CHECK(second.size() == 5);
    CHECK(second[2] == "free cash $975,125.00 ≥ $24,875.00 ✓");
    CHECK(second[3] == "locks $24,875.00 → free cash now $950,250.00");
}

void test_send_attribution_ignores_other_nodes()
{
    // BANK_C restarts between BANK_A's decision-wait firing (1,003,200) and
    // the release of the question it queued (1,003,300). The question still
    // belongs to BANK_A's timer handler.
    glassbox::ScenarioSetup setup =
        glassbox::make_s9_coordinator_crash_after_decision(1);
    setup.script.push_back(glassbox::TimedAction{
        1'000'000,
        glassbox::ScriptActionEvent{glassbox::ScriptCrash{NodeId::bank_c}}});
    setup.script.push_back(glassbox::TimedAction{
        1'003'250,
        glassbox::ScriptActionEvent{glassbox::ScriptRestart{NodeId::bank_c}}});
    const Recording rec = glassbox::cli::record(setup);
    bool checked = false;
    for (std::size_t i = 1; i < rec.frames.size(); ++i) {
        if (rec.frames[i].time_us != 1'003'300) {
            continue;
        }
        const Story s = glassbox::cli::narrate(rec, i);
        CHECK(s.headline == "BANK_A asks COORD: what was decided for trade #1?");
        CHECK(s.code == "Effects::release ← Participant::on_decision_wait");
        checked = true;
    }
    CHECK(checked);
}

// S11: COORD's retransmit fires at 1,073,300 and its COMMIT to BANK_A is
// released at 1,073,400. Another event reaches COORD at 1,073,350, in
// between; the COMMIT must still be credited to the retransmit handler.
void check_retransmit_credited(const Recording& rec)
{
    bool checked = false;
    for (std::size_t i = 1; i < rec.frames.size(); ++i) {
        for (const auto& r : rec.frames[i].records) {
            if (r.kind == "MSG_SEND" && r.sim_time_us == 1'073'400 &&
                r.to == NodeId::bank_a &&
                r.msg_type == glassbox::MessageType::commit) {
                CHECK(glassbox::cli::narrate(rec, i).code ==
                      "Effects::release ← Coordinator::on_retransmit");
                checked = true;
            }
        }
    }
    CHECK(checked);
}

void test_send_attribution_with_events_in_between()
{
    // In between: BANK_B's ACK (an ACK handler cannot send COMMIT). The
    // B -> COORD link slows down after the votes are in.
    glassbox::ScenarioSetup ack = glassbox::make_s11_lost_commit(1);
    ack.script.push_back(glassbox::TimedAction{
        100'000, glassbox::ScriptActionEvent{glassbox::ScriptSetLink{
                     NodeId::bank_b, NodeId::coordinator, 963'950, 0, 0}}});
    const Recording with_ack = glassbox::cli::record(ack);
    bool ack_between = false;
    for (const auto& frame : with_ack.frames) {
        for (const auto& r : frame.records) {
            ack_between = ack_between ||
                          (r.kind == "MSG_DELIVER" && r.sim_time_us == 1'073'350 &&
                           r.msg_type == glassbox::MessageType::ack);
        }
    }
    CHECK(ack_between);
    check_retransmit_credited(with_ack);

    // In between: the deciding vote of another trade (a vote handler can
    // send COMMIT, but for trade 2).
    glassbox::ScenarioSetup other = glassbox::make_s11_lost_commit(1);
    other.script.push_back(glassbox::submit(
        831'150,
        glassbox::standard_trade(NodeId::bank_a, NodeId::bank_c, 831'150)));
    const Recording with_vote = glassbox::cli::record(other);
    bool vote_between = false;
    for (const auto& frame : with_vote.frames) {
        for (const auto& r : frame.records) {
            vote_between = vote_between ||
                           (r.kind == "STATE_CHANGE" &&
                            r.sim_time_us == 1'073'350 &&
                            r.trade_id == glassbox::TradeId{2} &&
                            r.detail == "COLLECTING->COMMITTING");
        }
    }
    CHECK(vote_between);
    check_retransmit_credited(with_vote);
}

void test_every_frame_has_a_story()
{
    for (const glassbox::ScenarioInfo& info : glassbox::scenario_registry()) {
        for (std::uint64_t seed = 1; seed <= 3; ++seed) {
            const Recording rec = glassbox::cli::record(info.make(seed));
            for (std::size_t i = 1; i < rec.frames.size(); ++i) {
                const Story s = glassbox::cli::narrate(rec, i);
                CHECK(!s.headline.empty());
                CHECK(!s.code.empty());
                // No unknown node or raw record-kind fallbacks.
                CHECK(s.headline.find('?') == std::string::npos ||
                      s.headline.find("what was decided") != std::string::npos);
                CHECK(s.headline.find("TIMER_SET") == std::string::npos);
                CHECK(s.headline.find("MSG_") == std::string::npos);
                for (const std::string& d : s.details) {
                    CHECK(!d.empty());
                }
            }
        }
    }
}

} // namespace

int main()
{
    test_formatting();
    test_s1_story();
    test_refusals();
    test_timeouts_and_network();
    test_crash_and_recovery();
    test_check_boundaries_and_existing_locks();
    test_send_attribution_ignores_other_nodes();
    test_send_attribution_with_events_in_between();
    test_every_frame_has_a_story();
    return 0;
}
