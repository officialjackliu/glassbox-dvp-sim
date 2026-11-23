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

## Task 15: S1 happy path, scenario registry, minimal CLI

Built:
- `include/scenarios/scenario.hpp`: `ScenarioSetup {config, genesis, script}`,
  `TimedAction`, `ScenarioInfo {id, name, make(seed)}`, `scenario_registry()`,
  `find_scenario()`, and `ScenarioRun` (builds simulator + protocol nodes, schedules the
  script in order, keeps the full trace, optional echo sink). Shared defaults:
  1,000,000.00 cash and 25,000 bonds per bank; standard trade BANK_A buys 25 UST_10Y from
  BANK_B for 24,875.00, deadline 30 s after submission.
- `include/scenarios/scenario_library.hpp`: `make_s1_happy_path(seed)`.
- `Simulator::run_bounded(max_events)` → true on quiescence (scenario runs default to
  1,000,000 events, so a stuck protocol fails instead of hanging).
- `include/trace/trace_format.hpp`: one-line trace rendering for the CLI; never prints
  `measured_ns`.
- `src/main.cpp` replaced by the minimal CLI decided in CLAUDE.md: `--list`,
  `--scenario S1 [--seed N]`, one line per trace record, then final node states and
  `quiesced=yes|no`. Exit 0 / 2 (usage) / 3 (bound hit).
- CMake: new `glassbox_core` static library (all sources except `main.cpp`); new
  targets link it. Older test targets keep their explicit source lists (unchanged).

Decisions:
- Scenario outcomes are asserted in tests, not encoded in the registry (registry is data).
- CLI output has no swimlane formatting yet (Task 26).

Tests: `scenario_s1_test` (registry lookups incl. near-misses, defaults, S1 outcome with
exact balances and invariants 1–5, the full protocol-visible trace sequence with times
derived by hand from the latency/processing/flush constants, timer bookkeeping, 20 seeds
identical, exact `format_trace_line` strings, `run_bounded` boundary at exactly N events);
CLI ctests: `--list`, S1 quiesced, exact final BANK_A line, a specific trace line,
unknown scenario / bad seed / no args fail. Mutants: 12/12 killed.

## Task 16: S2 and S3 vote-no aborts

Built:
- `make_s2_buyer_lacks_funds` (BANK_A holds price − 1 cent) and `make_s3_account_frozen`
  (BANK_B genesis `frozen = true`), registered as S2/S3.
- `describe_body()` in `protocol/messages`: message trace records now carry a short body
  summary in `detail` (`YES`, `NO:<REASON>`, `BUYER`/`SELLER`, abort reason, ACK
  decision, `BUYER<-SELLER` for PROPOSE). Lets tests and the CLI show *why* without
  decoding bodies.

Decisions:
- **`freeze_account(n)` is modeled as the genesis frozen flag**, not a runtime script
  action. Reason: nodes only change state through messages/timers and replay starts from
  genesis; a runtime toggle would need a durable FROZEN record to survive restart. Simplest
  option consistent with DESIGN.md §4.1 ("frozen: bool, toggled by scenarios").
  Follow-up if a mid-run freeze is wanted: add a `SetFrozen` log record + admin message.

Tests (`scenario_vote_no_test`):
- S2: buyer VOTE `NO:INSUFFICIENT_FUNDS` at 3,200, tombstone only; seller LOCK at 36,100
  and UNLOCK at 40,300 (it had locked before ABORT arrived); all three ABORTs carry
  INSUFFICIENT_FUNDS (two broadcast + one reply to the seller's late yes); no COMMIT; no
  balance moved; invariants.
- S3: seller VOTE `NO:ACCOUNT_FROZEN`, no lock, tombstone; buyer LOCK 2,100 / UNLOCK
  74,300; ABORT reason ACCOUNT_FROZEN; ACK at 75,400.
- Other side of the S2 boundary: buyer with exactly the price commits (cash → 0).
- Restart after quiescence (crash + restart both banks at 20 s, seeds 1/2/99): same states
  and ledgers, no recovery errors, no outcome queries.
- Registry entries change only the intended genesis field.
- `messages_test`: exact `describe_body` table; COMMIT trace has no detail.

Mutants: 7/7 killed across `scenario_vote_no_test` and `messages_test` (2 only as
compile-checked table entries).
