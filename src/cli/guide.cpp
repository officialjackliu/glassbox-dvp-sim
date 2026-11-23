#include "cli/guide.hpp"

namespace glassbox::cli {

namespace {

constexpr std::array<ScenarioGuide, 14> k_guides{{
    {"S1", "Happy path: both legs settle",
     "BANK_A in New York buys 25 bonds from BANK_B in London. The coordinator "
     "asks both banks to lock their side, collects two YES votes, writes "
     "COMMIT to disk and tells both banks. Cash and bonds change hands "
     "together.",
     "Each bank locks before it votes, and no message leaves before the "
     "record behind it is on disk."},
    {"S2", "Buyer lacks funds",
     "BANK_A holds one cent less than the price. It refuses to lock, writes "
     "ABORTED and votes NO. The coordinator aborts and BANK_B releases the "
     "bonds it had locked.",
     "One NO cancels the whole trade: nothing moves at either bank."},
    {"S3", "Seller's account is frozen",
     "BANK_B's account is frozen, so it refuses the trade and votes NO. "
     "BANK_A has already locked its cash; it gets ABORT and releases it.",
     "A refusal by one side undoes the other side's lock."},
    {"S4", "Slow link, tight timeout",
     "BANK_A buys from BANK_C in Singapore over a link slowed to 300 ms each "
     "way. The 500 ms vote timer runs out before BANK_C's YES arrives, so "
     "the coordinator aborts even though both banks agreed.",
     "A false abort: safe, but wasted work. Then try a longer timer: "
     "make watch S=S4 SET=\"vote_timeout=900ms\""},
    {"S5", "A bank is offline before voting",
     "BANK_B is offline from the start and comes back at 3 s. Its PREPARE is "
     "lost, the vote timer runs out and the trade aborts. When BANK_B "
     "returns, a resent ABORT reaches it and it records ABORTED.",
     "Silence counts as NO, and the coordinator resends until every bank "
     "confirms."},
    {"S6", "A bank crashes after voting YES",
     "BANK_A votes YES and crashes right after. The coordinator commits "
     "while BANK_A is down for 5 s. On restart BANK_A reads PREPARED from "
     "disk, restores its lock, asks for the outcome and settles.",
     "The lock survives the crash because the promise was on disk."},
    {"S7", "A bank crashes before its vote leaves",
     "BANK_A writes PREPARED to disk but crashes before its vote is sent. The "
     "coordinator hears nothing and aborts. BANK_A restarts in doubt, asks, "
     "and learns ABORT.",
     "The disk says 'I promised', but the network never heard it."},
    {"S8", "A bank crashes before writing anything",
     "BANK_A crashes before anything reaches its disk and restarts with no "
     "memory of the trade. The coordinator aborts; a resent ABORT reaches "
     "BANK_A, which records ABORTED so a late PREPARE would be refused.",
     "Nothing on disk means nothing to recover."},
    {"S9", "The coordinator crashes after deciding",
     "Both banks vote YES. The coordinator writes COMMIT to disk and crashes "
     "before telling anyone. For 5 s both banks hold their locks and keep "
     "asking, unanswered. On restart the coordinator finds COMMIT on disk "
     "and resends it.",
     "Banks that voted YES may not decide alone: this is why two-phase "
     "commit is called blocking."},
    {"S10", "The coordinator crashes mid-announcement",
     "The coordinator crashes while announcing COMMIT: BANK_A hears it, "
     "BANK_B does not. BANK_A settles at once; BANK_B waits in doubt until "
     "the coordinator restarts and resends.",
     "Both banks settle the same way, just at different times."},
    {"S11", "A COMMIT is lost",
     "The first COMMIT to BANK_A is lost on the network. The coordinator "
     "gets no ACK, resends after 1 s, and the second copy settles BANK_A.",
     "Resending until confirmed makes a lost message harmless."},
    {"S12", "Duplicated and reordered messages",
     "The links have high jitter, and PREPARE and COMMIT are each delivered "
     "twice. Messages overtake each other and arrive again.",
     "Duplicates never lock twice or settle twice."},
    {"S13", "Two trades, one set of bonds",
     "BANK_A and BANK_C both buy from BANK_B, which has bonds for only one "
     "trade. The first PREPARE wins the lock; the second is refused for lack "
     "of free bonds and aborts.",
     "Locks stop the same bonds from being sold twice."},
    {"S14", "Network partition",
     "BANK_A buys from BANK_C. Just after BANK_C locks, the network between "
     "the coordinator and BANK_C is cut. BANK_C's YES never arrives and the "
     "trade aborts; after the link is restored at 2 s, BANK_C asks and "
     "learns ABORT.",
     "A partition is not a crash: everyone keeps their memory, only "
     "messages are lost."},
}};

} // namespace

const ScenarioGuide* find_guide(std::string_view id) noexcept
{
    for (const ScenarioGuide& guide : k_guides) {
        if (guide.id == id) {
            return &guide;
        }
    }
    return nullptr;
}

} // namespace glassbox::cli
