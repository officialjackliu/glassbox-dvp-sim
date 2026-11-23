# Overnight log: `claude_vibe_code` branch

Running log of autonomous work on Tasks 11–23 of `BUILD_PLAN.txt`. All work is on
`claude_vibe_code`; nothing is pushed to or merged into `main`.

## Environment notes (read first)

- **Local toolchain is damaged.** `/Library/Developer/CommandLineTools/usr/include/c++/v1`
  holds only 12 entries (no `<concepts>`, `<cstdio>`, `<vector>`). Clang searches it before
  the SDK, so nothing compiles out of the box. All builds below add
  `-nostdinc++ -isystem $(xcrun --show-sdk-path)/usr/include/c++/v1` to `CMAKE_CXX_FLAGS`
  (build dirs live in the session scratchpad; the repo's `build/` dirs are untouched).
- **ASan cannot run on this machine.** An empty `int main(){}` built with
  `-fsanitize=address` hangs at startup (tested inside and outside the sandbox). So the
  "ASan+UBSan" leg of the three-build rule was run as **UBSan only**, with
  `-fno-sanitize-recover=undefined` so any UB fails the test. **ASan has not been run on
  any commit on this branch.** Fix: `sudo rm -rf /Library/Developer/CommandLineTools &&
  xcode-select --install`, then rerun the three CLAUDE.md commands.
- Per-task gate used: Debug `-Werror`, Release `-Werror`, Debug+UBSan `-Werror`, all with
  `ctest --output-on-failure`. Mutation testing with a scratch script (plant one-line
  bugs, rebuild the task's test target, confirm it fails); results are listed per task.

## Task 10: DurableLog and replay (committed before this log started)

- The Task 10 work on `task-10-durable-log-replay` was uncommitted; built and tested
  (Debug, 12/12) and committed as `4d055d1` "Implement Task 10 DurableLog and replay",
  pushed to that branch.
  `claude_vibe_code` was branched from it, so it contains Tasks 7–10 (stacked, as the
  user merges to `main` themselves).

## Task 11: protocol message types and trace-carrying IDs (`9b12b7d`)

Built:
- `include/protocol/messages.hpp`: `Message` moved here from `event_queue.hpp` and given a
  typed `body` variant (`ProposeBody{request}`, `PrepareBody{trade, leg}`,
  `VoteBody{vote, reason}`, `CommitBody`, `AbortBody{reason}`, `AckBody{decision}`,
  `OutcomeQueryBody`). `make_*` builders; `check_message()` with fixed order: body
  present → body matches type → trade_id presence → PREPARE trade_id matches body →
  route (roles, and PREPARE goes to the leg's owner) → vote reason → abort reason.
- Network and simulator carry the whole `Message`. The old field overload of
  `schedule_delivery` delegates, so RNG draw order is unchanged (tested).
- Simulator assigns `msg_id` at send release when a node leaves it 0 (counter from 1).
- `ScriptSubmitTrade{TradeRequest}` script action delivers PROPOSE straight to the
  coordinator (client is co-located; no RNG draw).
- `TraceRecord` gained optional `node, trade_id, msg_type, msg_id, from, to, log_record,
  detail`; `MSG_DROP` now carries its reason (`DROP_RANDOM/PARTITION/NODE_DOWN/DROP_NEXT`);
  `TraceNote` carries `trade_id` and `handler`.
- `to_string` tables for `ReasonCode, TradeState, Leg, TimerKind, DropReason, Vote,
  Decision, MessageCheck`.

Decisions:
- **`Node::on_log_durable(record, now)`** (default no-op) is called by the simulator
  after each flushed record. Reason: the batch rule alone cannot stop a *duplicate*
  PREPARE or a late vote that arrives during a flush from triggering a reply in a
  log-free batch, which would leave before the record behind it is durable. Rule
  adopted: never announce a yes vote or a decision before its record is durable. It
  returns no Effects, so batch semantics and crash points are unchanged.
- Messages without a body (`std::monostate`) remain allowed for legacy tests but are
  rejected by `check_message`, so protocol nodes ignore them.

Tests: `messages_test`: every builder field, all 42 type/body mismatches, every
rejection, check order, all string tables, all 7 message types routed through
network and simulator with bodies intact and unique msg_ids, drop trace contents,
PROPOSE to a down coordinator. Mutants: 18/18 killed.

## Task 12: coordinator state machine (`7433742`)

Built `include/protocol/coordinator.hpp` / `src/protocol/coordinator.cpp`, following the §5.3 table:
PROPOSE validate → PROPOSED + PREPARE×2 (buyer first) + VOTE_TIMEOUT
`min(vote_timeout, valid_until − now)`; yes+yes → DECISION_COMMIT + COMMIT×2 +
RETRANSMIT; any no / timeout → DECISION_ABORT{reason} + ABORT×2; ACK both → END, DONE;
RETRANSMIT resends to non-acked parties with saturating attempt counter; OUTCOME_QUERY:
decided+durable → decision, collecting or not-yet-durable → no reply, unknown → ABORT;
restart: replay, `observe(max_trade_id)`, restore decided trades (resend + retransmit
unless ended), forget undecided ones.

Decisions:
- Votes/ACKs/queries from a node that is not a party are ignored with an
  `INVALID_MESSAGE/NOT_A_PARTY` note.
- ACK with the wrong decision → `INVARIANT_VIOLATION/ACK_DECISION_MISMATCH` note, no
  state change. ACK before any decision → `ACK_IGNORED/NO_DECISION`.
- ABORT replies always carry a reason; presumed abort uses `UNKNOWN_TRADE`.

Tests: `coordinator_test`: table tests for each state × event, timeout-cap boundaries
(−1/0/+1), backoff steps and saturation, restart idempotence and id reuse prevention;
simulator runs with scripted participants (commit with exact timings, vote-no,
false-abort timeout + late vote, lost COMMIT retransmit, live `on_log_durable`, crash
before vs after the decision flush). Mutants: 32/32 killed (one survivor found a gap:
retransmit when only the seller had acked; test added).

Test-harness fix during development: the scripted participant attached the abort reason
to *yes* votes, which `check_message` correctly rejected. Fixed the harness (not an
assertion).

## Task 13: participant state machine (`9c35c3d`)

Built `include/protocol/participant.hpp` / `src/protocol/participant.cpp`, following the §5.4 table.
PREPARE checks in fixed order frozen → expired → `available >= amount`; pass → lock via
`LedgerOp`, PREPARED + VOTE yes + DECISION_WAIT in one batch, state PREPARING, then
PREPARED when the record is durable; fail → ABORTED tombstone + VOTE no(reason).
COMMIT/ABORT/duplicates/invalid states per the table; DECISION_WAIT sends OUTCOME_QUERY
and never aborts; restart replays from genesis and queries every in-doubt trade.

Decisions:
- Vote-no writes an ABORTED tombstone (makes the refusal survive a restart).
- Duplicate PREPARE while PREPARING → no reply (yes already queued); in COMMITTED → yes.
- Any other ledger lock refusal maps to the leg's insufficient-asset reason (safe "no").
- Unqualified `apply(ledger, op)` resolved to `std::apply` by ADL; calls are qualified
  `glassbox::apply`.

Tests: `participant_test`: exact Effects and ledger for each state × event, both sides of
balance and deadline boundaries, check precedence, unchanged state for every failure
path, bond contention, restart idempotence and replay errors; simulator runs proving the
vote leaves only after the PREPARED flush (2,200 vs 2,100), PREPARING→PREPARED at flush
time, in-doubt query backoff times (1,002,300 / 3,002,400 / 7,002,500 / 15,002,600),
crash before flush loses the lock. Mutants: 37/37 compilable killed.

## Task 14: integrate coordinator and participants

Built:
- `include/scenarios/protocol_system.hpp`: `install_protocol_nodes(simulator, BankGenesis)`
  installs one Coordinator and three Participants configured from
  `simulator.config().timers`, returns non-owning observers.
- `Simulator::config()` accessor.
- `node_purity_test` now also greps `src/protocol/*.cpp` for simulator/network/RNG/clock
  includes (no cross-node or simulator access from protocol code).

Tests (`protocol_integration_test`):
- S1 happy path end to end: exact message counts per type (2 each of PREPARE, VOTE,
  COMMIT, ACK; 0 others; 9 deliveries incl. PROPOSE), per-node log sequences, every
  durable/delivery time, exact balances, conservation, zero locks, all 4 timers ignored
  by state check, exact STATE_CHANGE sequence with handler names, and a trace-wide
  write-ahead check (each node's first VOTE/ACK/COMMIT/ABORT/PREPARE for a trade comes
  after the LOG_APPEND that backs it).
- Vote-no (buyer one cent short): seller locks then releases, tombstone on buyer.
- Lost COMMIT resolved by the participant's OUTCOME_QUERY before the retransmit; the
  duplicate COMMIT is harmless (one COMMITTED, one END, one ACK_IGNORED).
- Two concurrent trades on different pairs; rejected PROPOSE sends nothing.
- Timer config wiring with distinct vote-timeout / retransmit / decision-wait values.
- Determinism: same seed ⇒ identical trace (all fields except `measured_ns`) under
  jitter + 10 % random drops; a different seed differs; 200 seeds keep atomicity,
  conservation and no leaked locks.

Mutants: 9/9 killed (3 by non-termination: with the flush callback removed the
coordinator retransmits forever). One survivor found that the vote timeout passed to the
coordinator was not checked; the wiring test was added.

Uncertain / follow-up: a stuck protocol makes `Simulator::run()` loop forever (there is
no event or time bound). Fine for correct code, but a bounded run would make failing
tests fail fast instead of hanging.
