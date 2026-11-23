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

## Task 17: S4 slow link / tight timeout and S5 participant down

Built:
- `make_s4_slow_link_tight_timeout(seed)` / `make_s4_with_vote_timeout(seed, timeout)`:
  BANK_A buys from BANK_C, COORD↔BANK_C set to 300 ms each way by script at t=0.
- `make_s5_participant_down(seed)`: `node_down(BANK_B)` at t=0, `node_up` at 3 s.
- Registered S4, S5.

Decisions:
- S4 uses BANK_C (Singapore) as the seller so the slow link is on the trade's path; the
  "slow" latency is a script `set_link` at t=0 (both directions), not a config change.
- S5 uses node_down/node_up as DESIGN.md says; a crash/restart variant is tested too and
  gives the same drops and resolution. Note for DESIGN §7.2 (still open from CLAUDE.md):
  with node_down the node keeps volatile state and its timers fire as `node_down`-ignored;
  with crash the epoch bumps and timers are `stale_epoch`-ignored.

Tests (`scenario_timeout_test`):
- S4 false abort: no crash, no drops; C's yes leaves 302,200 and arrives 602,200; the
  timeout fires at 501,100; ABORT(VOTE_TIMEOUT) to A and C at 502,200 and again to C at
  602,300 (late yes); unlocks exactly at first ABORT arrival (503,200 / 802,200), so no
  unilateral abort after yes; END at 1,104,300; no balance moved.
- S4 raised timeout (1 s) commits with exact balance change.
- Boundary: timeout 601,099 and 601,100 abort (the timer wins the tie at 602,200 by seq),
  601,101 commits.
- Deadline cap: 1 s timeout but 400 ms deadline → timer at 401,100, VOTE_TIMEOUT abort.
- S5: drops `PREPARE/ABORT/ABORT NODE_DOWN` at 36,100 / 537,200 / 1,537,300; A unlocks at
  503,200; retransmits at 1,502,200 and 3,502,300 target only BANK_B; B gets ABORT at
  3,537,400, writes a tombstone (durable 3,538,400), ACKs; END at 3,574,500; B never
  locked or voted. Same for the crash/restart variant and seeds 2–3.

Mutants: 5/5 compilable killed (+1 unused-parameter mutant checked directly).

## Task 18: S13 lock conflict

Built: `make_s13_lock_conflict(seed)`: BANK_B holds 40 bonds; trade 1 (A buys 25) and
trade 2 (C buys 25) are submitted at t=0 in that order. Registered as S13.

Decisions:
- Winner rule is simply event order: both PREPAREs arrive at BANK_B at 36,100 and the
  lower-seq one (first submitted) locks first. No special arbitration or global ledger.

Tests (`scenario_lock_conflict_test`):
- Right after 36,100: 25 bonds locked, 15 available; trade 1 PREPARING, trade 2 ABORTED.
- B votes `YES` (trade 1) and `NO:INSUFFICIENT_BONDS` (trade 2) at 37,200; decisions
  "1 COMMIT", "2 ABORT INSUFFICIENT_BONDS"; seller log PREPARED(1), ABORTED(2),
  COMMITTED(1) in LSN order; loser's buyer C locks at 121,100 and unlocks at 193,300;
  exact final balances; invariants.
- Reversed submission order flips the winner (C commits, A untouched).
- Capacity boundaries: 24 bonds → 0 commits, 25 → 1, 49 → 1, 50 → 2.
- Seeds 1–5 identical.

Test fix during development: in the reversed case I expected the coordinator to log the
commit before the abort; it logs decisions in time order (abort at 72,200, commit at
242,200 after C's yes crosses the 120 ms link). Corrected the expectation in the new test
and added the exact decision durable times (73,200 / 243,200).

Mutants: 3/3 killed (seller bonds 40→50, second submission delayed 1 µs, and the ledger's
`available < amount` flipped to `<=`).

## Task 19: crash points and node restart

Built:
- `include/faults/crash_spec.hpp`: `CrashPoint {BEFORE_LOG, AFTER_LOG_BEFORE_SEND,
  AFTER_FIRST_SEND}`, `CrashSpec {node, trade_id?, trigger (MessageType | TimerKind),
  point, restart_after_us?, occurrence}`.
- `Simulator::add_crash_spec()` (rejects occurrence 0 and negative restart delay),
  `pending_crash_specs()`. Specs are one-shot and matched on message delivery / timer fire
  against the batch the handler returns (`apply_effects_impl`):
  - BEFORE_LOG: crash immediately; nothing from the batch is logged, sent, or timed.
  - AFTER_LOG_BEFORE_SEND: records flush as usual; a crash event is scheduled at the flush
    time, after the flush event; sends and timers of the batch are never scheduled.
  - AFTER_FIRST_SEND: flush, only the first send is scheduled, crash event right after
    its release.
  - `restart_after_us` schedules a restart that long after the crash time.
- CRASH trace records carry the crash point as `reason`; `NodeCrashEvent` has an optional
  reason. `ScenarioSetup.crashes` registers specs for scenario runs.

Decisions:
- **`occurrence` field** (fire on the N-th match, default 1) added to the DESIGN.md
  CrashSpec, so S9 can target "the deciding VOTE" (the second VOTE of a trade).
- Lost sends/timers are *not scheduled* rather than scheduled-then-epoch-dropped, so the
  semantics hold even with `processing_delay_us = 0` (flush, first send and crash at the
  same instant still run in that order; tested).
- AFTER_LOG_BEFORE_SEND on a batch with no logs crashes immediately (nothing to wait for);
  AFTER_FIRST_SEND on a batch with no sends behaves like AFTER_LOG_BEFORE_SEND.
- Multiple specs matching one event: each counts the event; the first in registration
  order that reaches its occurrence fires.

Tests (`crash_points_test`): exact BANK_A event sequences (LOG_APPEND / MSG_SEND /
TIMER_SET / CRASH reason / RESTART with times) for no-crash, each crash point, zero
processing delay, and no-log batches; matching by trade id, occurrence 2 (fires once),
wrong node/type, timer trigger; invalid specs rejected; pre-crash timer ignored with
`stale_epoch` after restart; message to a down node dropped `NODE_DOWN`; restart sees the
durable log; protocol: BANK_A crashes right after its yes (restart 2 s later, in doubt,
queries, commits) plus coordinator crash/restart at quiescence; **recovery idempotence**:
crash+restart all four nodes once and twice at quiescence for S1, S2, S4, S13 gives the same
recoverable state (trade states, decisions, acks, ledgers), no new messages, no recovery
errors. Mutants: 15/15 killed.

Incident: my first version of the probe bench had the BANK_B receiver inherit the probe's
"send two messages" behaviour, sending to itself, so the message count grew exponentially.
The test ran ~10 min, swapped, and filled the disk (ENOSPC). I stopped it, freed my own
scratch ASan build dir, fixed the receiver to only count, and now run new test binaries
under a 30 s alarm first and ctest with `--timeout`. Two expectations in that new test were
also wrong (I forgot the timer batch's own flush + processing delay); corrected after
printing the actual event list.

## Task 20: S6–S8 participant crash and recovery

Built: `make_s6_in_doubt_participant` (BANK_A AFTER_FIRST_SEND on PREPARE, restart after
5 s), `make_s7_crash_before_vote_sent` (AFTER_LOG_BEFORE_SEND, restart after 2 s),
`make_s8_crash_before_log` (BEFORE_LOG, restart after 2 s), all on trade 1 via
`ScenarioSetup.crashes`. Registered S6–S8 (registry stays in id order, tested).

Decisions: restart delays chosen so that each scenario shows its intended path
(S6: in-doubt beyond two COMMIT retransmits; S7: participant queries before the
coordinator's second ABORT retransmit; S8: second ABORT retransmit finds the clean node).
Outcomes are also checked for restart delays 1 ms / 100 ms / 2.5 s / 9 s.

Tests (`scenario_participant_crash_test`):
- S6: crash at 3,200 after the yes leaves; just after restart (5,003,200) the lock and
  PREPARED state are restored from the log; three COMMIT drops (74,300 / 1,074,400 /
  3,074,500); OUTCOME_QUERY 5,003,300 → COMMIT 5,005,400 → APPLY → COMMITTED 5,006,400
  → ACK → END 5,008,500; lock held from 2,100 to 5,005,400 (spans the outage).
- S7: crash at 3,100 with PREPARED durable and no vote sent; VOTE_TIMEOUT abort
  (DECISION_ABORT 502,100); two ABORT drops; BANK_B unlocks 537,200; BANK_A queries
  2,003,200, gets ABORT(VOTE_TIMEOUT) at 2,004,300 send, unlocks 2,005,300; END 2,008,400.
- S8: crash at 2,100 before logging; after restart no record, no trade, no lock, no
  query; second ABORT retransmit arrives 3,503,400 → tombstone durable 3,504,400; END
  3,506,500.
- Outcome independent of restart delay (4 values × 3 scenarios), invariants everywhere.

Mutants: 5/5 killed (crash points swapped between scenarios, restart query removed,
replayed ledger ignored, coordinator outcome-query reply removed).

## Task 21: S9–S12 coordinator and communication failures

Built:
- S9 `make_s9_coordinator_crash_after_decision`: coordinator AFTER_LOG_BEFORE_SEND on the
  2nd VOTE of trade 1 (`occurrence = 2`), restart after 5 s.
- S10 `make_s10_coordinator_crash_mid_broadcast`: same trigger, AFTER_FIRST_SEND.
- S11 `make_s11_lost_commit`: `drop_next(COMMIT, COORD→BANK_A)`, decision-wait start 2 s.
- S12 `make_s12_duplicates_and_reordering`: COORD↔A 20 ms ± 19 ms, COORD↔B 35 ms ± 30 ms,
  and duplicate the next PREPARE and COMMIT on both coordinator→bank links.
- `NetworkModel::duplicate_next(from, to, type)` and `ScriptDuplicateNext`: the next
  matching message is delivered twice; the copy keeps its msg_id and consumes its own
  jitter + drop draws right after the original's.

Decisions:
- **S11 changes one dial** (`decision_wait.initial_us = 2 s`). With the defaults, BANK_A's
  own OUTCOME_QUERY (1 s) resolves the lost COMMIT before the coordinator's retransmit
  (1 s after its later send time). DESIGN.md says "retransmit delivers it", so S11 makes
  that the path; the query path is already covered in `protocol_integration_test`.
- **Duplication** is a new network injector (DESIGN.md §7.2 had only `drop_next`);
  simplest model of a duplicated packet. A duplicate on a partitioned link is dropped twice.

Tests (`scenario_coordinator_failure_test`):
- S9: crash at 73,200 with DECISION_COMMIT durable; at 4 s both participants PREPARED with
  locks held and no COMMIT ever sent (blocking); four OUTCOME_QUERYs dropped NODE_DOWN
  (1,004,300 / 1,072,300 / 3,004,400 / 3,072,400); restart 5,073,200 → RESEND_DECISION →
  COMMITs at 5,073,300 → deliveries 5,074,300 / 5,108,300 → END 5,145,400.
- S10: crash at 73,300 right after the COMMIT to BANK_A; BANK_A committed, BANK_B in doubt
  at 4 s; drops: A's ACK and B's two queries; restart resends to both (A's copy is a
  duplicate: one APPLY, two ACKs); END 5,145,500.
- S11: DROP_NEXT at 74,300; RETRANSMIT to BANK_A only at 1,073,300; COMMIT delivered
  1,074,400; END 1,077,500; no outcome query.
- S12 over 300 seeds: always commits with exact balances, no drops, no invalid messages;
  each bank gets 2 PREPAREs and ≥ 2 COMMITs, locks once and applies once; across seeds
  both duplicate-PREPARE paths occur (ignored while PREPARING, yes resent after), copies
  arrive at different times in > 500 of 600 cases; same seed ⇒ same trace.
- `duplicate_next` unit tests: same msg_id, draw order equals two real sends, one-shot,
  type / direction / destination matching, partition drops both copies.
- S9/S10 outcome independent of restart delay (1 ms, 0.9 s, 2 s, 12 s).

Mutants: 7/7 killed after adding a destination-only matching case (the survivor showed the
`to` check was untested).
