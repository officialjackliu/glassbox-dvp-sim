# Glass-Box DvP Simulator: Design Document

A local, deterministic simulation of **atomic Delivery-vs-Payment (DvP) settlement across a distributed ledger**, built so every step (message, lock, log write, timeout, crash, recovery) is visible and can be mapped back to the code that caused it.

> **Status:** design only. Implement milestone by milestone (Section 15). Do not skip the tests in each milestone.
> **Audience:** the engineer or coding agent implementing this in C++20.

---

## 0. Framing and honesty

- This is an **independent, educational, open-source simulator**. It is *inspired by* public descriptions of Canton-style multi-party atomic settlement (participant nodes, a sequencer, a mediator/coordinator, two-phase-commit-style transaction protocol). It is **not** an implementation of any vendor's system and makes no claim to match one.
- Do not use any company's name or branding in the repo name, code identifiers, or UI. Suggested project name: `glassbox-dvp-sim`.
- Do not state performance numbers that were not measured. All throughput or latency figures in docs must come from a run of the program.
- Section 17 lists what is simplified compared to real systems. Keep it in the README.

---

## 1. Goals and non-goals

### Goals
1. Simulate **three participant nodes plus a coordinator** that must agree on a trade atomically over a faulty, delayed network.
2. Show **successful and failed** trades, with each failure having a *different, correct* cleanup path.
3. Make **latency by region** a real, adjustable factor, with simulated network time kept visibly separate from measured compute time.
4. Be **deterministic and replayable**: same seed + same scenario = same trace.
5. Provide a **trace** rich enough to render a swimlane diagram, a per-trade waterfall, and per-node state views.
6. Expose **dials** (latency, timeouts, drop rate, crash points, load) and **metrics** (abort rate, settlement latency, lock-hold time, in-doubt time).

### Non-goals (v1)
- Real networking, real threads in the simulation core, real cryptography.
- Byzantine fault tolerance or a real consensus algorithm.
- Multiple assets, partial fills, netting, privacy/sub-transaction views.
- Matching the performance of a production system.

---

## 2. Architecture overview

```
┌──────────────────────────────────────────────────────────────────┐
│ Scenario runner / CLI  (scripts: submit trade, crash, set latency)│
├──────────────────────────────────────────────────────────────────┤
│ Simulator core  (virtual clock, event queue, seeded RNG)          │
│   ├── Network model  (per-link latency, jitter, drop, partition)  │
│   ├── Fault injector (crash points, node down/up)                 │
│   └── Trace + Metrics sinks                                       │
├──────────────────────────────────────────────────────────────────┤
│ Nodes (pure state machines, no threads, no real time)             │
│   ├── Coordinator  (also sequences trades in v1)                  │
│   └── Participant × 3  (BANK_A, BANK_B, BANK_C; each owns a ledger │
│                         and a durable log)                        │
└──────────────────────────────────────────────────────────────────┘
```

Key properties:
- **Single-threaded discrete-event simulation.** One loop pops the earliest event, delivers it, and schedules the consequences. No mutexes, no real sleeps, no ring buffer.
- **Nodes are state machines.** They receive a message or timer and return *effects* (log records to write, messages to send, timers to set). They never touch the network, clock, or other nodes directly.
- **Nodes never read each other's state.** All knowledge crosses via messages.

---

## 3. Simulation core

### 3.1 Time and units
- `SimTimeUs = int64_t` microseconds of **virtual** time. Starts at 0.
- Money: `int64_t` cents. Quantities: `int64_t` units. **Never use floating point for state.**
- Real (wall-clock) time is used *only* to measure how long handler code took (`measured_ns`, from `std::chrono::steady_clock`). It never influences simulation logic.

### 3.2 Event queue
- Min-heap ordered by `(time_us, seq)`, where `seq` is a monotonically increasing insertion counter. This makes ordering of simultaneous events deterministic.
- Event kinds: `DELIVER_MESSAGE`, `FIRE_TIMER`, `NODE_CRASH`, `NODE_RESTART`, `SCRIPT_ACTION` (scenario step such as "submit trade" or "change latency").
- Timers are **not cancelled**. Each timer carries `(trade_id, kind, expected_state)`. When it fires, the handler checks the trade is still in `expected_state` and does nothing otherwise.

### 3.3 Randomness
- One `std::mt19937_64` seeded from the scenario seed. All randomness (jitter, drops) draws from it in a fixed order.
- **Determinism rule:** with `measured_ns` excluded, identical seed + scenario must produce a byte-identical trace. Add a test for this.

### 3.4 Network model
- Config per directed link `(from, to)`: `base_latency_us`, `jitter_us` (uniform in `[-j, +j]`, clamped so latency ≥ 1), `drop_probability`.
- Switches: `partitioned(a, b)` (drop all), `node_down(n)` (drop all inbound while down).
- Delivery time = `send_time + latency`. Because of jitter, **messages may be reordered**. Handlers must tolerate that (see 6.3).
- Illustrative default one-way latencies (ms): NY↔London 35, NY↔Singapore 120, London↔Singapore 85, intra-region 1. These are placeholders for the demo, not measurements. Make them configurable.
- Default placement: coordinator in NY; BANK_A in NY; BANK_B in London; BANK_C in Singapore.

### 3.3 Node interface (suggested)

```cpp
struct Effects {
  std::vector<Outgoing>  send;     // messages to send
  std::vector<LogRecord> log;      // records to append durably
  std::vector<TimerReq>  timers;   // timers to schedule
  std::vector<TraceNote> notes;    // extra trace detail (reasons, state changes)
};

class Node {
 public:
  virtual Effects on_message(const Message&, SimTimeUs now) = 0;
  virtual Effects on_timer(const TimerEvent&, SimTimeUs now) = 0;
  virtual void    on_crash() = 0;                 // wipe volatile state
  virtual Effects on_restart(const std::vector<LogRecord>& durable_log,
                             SimTimeUs now) = 0;
};
```

### 3.4 Write-ahead rule, enforced structurally
The simulator applies an `Effects` batch like this:
1. Append all `log` records to the node's **durable log**. They become durable at `now + log_flush_delay_us`.
2. Release all `send` messages at `now + processing_delay_us + (log.empty() ? 0 : log_flush_delay_us)`.

So **no message can leave a node before the log records produced in the same step are durable.** This makes "log before you speak" impossible to violate by accident, and it gives crash points (Section 9) a precise meaning.

Defaults: `processing_delay_us = 100`, `log_flush_delay_us = 1000`. Both configurable (a slow-disk dial).

---

## 4. Domain model

### 4.1 Node, account, ledger
Each participant node owns exactly one account in v1:

```
LocalLedger {
  cash_total, cash_locked         // cents
  bonds_total, bonds_locked       // units of asset "UST_10Y"
  frozen: bool                    // compliance flag, toggled by scenarios
  available_cash  = cash_total  - cash_locked
  available_bonds = bonds_total - bonds_locked
}
```

Initial balances come from the scenario config (genesis). **Current balances are always derivable from genesis + the durable log.**

### 4.2 Trade
```
Trade {
  trade_id            // assigned by the coordinator on PROPOSE
  buyer, seller       // node ids
  asset               // "UST_10Y"
  qty                 // units
  cash_cents          // price leg
  valid_until_us      // business deadline in sim time
  proposer            // who asked for it (informational)
}
```

### 4.3 Legs and locks
- **Buyer's leg:** lock `cash_cents` of cash. On commit: debit locked cash, credit `qty` bonds.
- **Seller's leg:** lock `qty` bonds. On commit: debit locked bonds, credit `cash_cents` cash.
- Credits need no lock. A node only ever changes its **own** ledger.
- Locking succeeds only if `available >= amount`. Multiple trades can be in flight; locks reduce availability, which gives lock conflicts naturally.

---

## 5. Protocol (two-phase commit with presumed abort)

### 5.1 Messages
| Type | Direction | Payload |
|---|---|---|
| `PROPOSE` | scenario/client → Coordinator | trade fields |
| `PREPARE` | Coordinator → buyer, seller | trade, which leg the node owns |
| `VOTE` | participant → Coordinator | `yes` or `no` + reason code |
| `COMMIT` | Coordinator → participant | trade_id |
| `ABORT` | Coordinator → participant | trade_id, reason |
| `ACK` | participant → Coordinator | trade_id, which decision |
| `OUTCOME_QUERY` | participant → Coordinator | trade_id |

The reply to `OUTCOME_QUERY` is simply a `COMMIT` or `ABORT` message. Every message gets a unique `msg_id` (for tracing only). Handlers are idempotent by *state*, not by message id.

Vote/abort reason codes: `INSUFFICIENT_FUNDS`, `INSUFFICIENT_BONDS`, `ACCOUNT_FROZEN`, `EXPIRED`, `VOTE_TIMEOUT`, `ALREADY_ABORTED`, `UNKNOWN_TRADE`.

### 5.2 Durable log records
| Node | Record | Written when |
|---|---|---|
| Participant | `PREPARED{trade, leg, locked_amount}` | after locking, before voting yes |
| Participant | `COMMITTED{trade, cash_delta, bond_delta}` | after applying commit |
| Participant | `ABORTED{trade}` | after releasing a lock on abort (also written as a tombstone, see 6.3) |
| Coordinator | `DECISION_COMMIT{trade, participants}` | **before** sending any COMMIT |
| Coordinator | `DECISION_ABORT{trade, reason}` | when deciding abort (needed only for tracing under presumed abort) |
| Coordinator | `END{trade}` | when all ACKs received |

**Presumed abort:** if the coordinator has no `DECISION_COMMIT` for a trade, the answer is ABORT. This is safe because a commit decision is always logged before it is announced.
The coordinator never garbage-collects decision records in v1 (`END` only stops retransmission).

### 5.3 Coordinator state machine (per trade)

States: `COLLECTING`, `COMMITTING`, `ABORTING`, `DONE`. (Volatile; reconstructed from the log on restart.)

| Event | State | Action |
|---|---|---|
| `PROPOSE` | none | Validate (nodes exist, qty > 0, deadline in future). Invalid: trace `REJECTED`, stop. Else assign `trade_id`, send `PREPARE` to buyer and seller, set `VOTE_TIMEOUT`, state → `COLLECTING`. |
| `VOTE yes` | `COLLECTING` | Record. If both yes: log `DECISION_COMMIT`, send `COMMIT` to both, set `RETRANSMIT`, state → `COMMITTING`. |
| `VOTE no` | `COLLECTING` | Log `DECISION_ABORT`, send `ABORT` to both, set `RETRANSMIT`, state → `ABORTING`. |
| `VOTE_TIMEOUT` | `COLLECTING` | Same as `VOTE no` with reason `VOTE_TIMEOUT`. |
| `VOTE` (any) | `COMMITTING`/`ABORTING`/`DONE` | Late or duplicate: re-send the existing decision to that voter. |
| `VOTE` (any) | unknown trade | Reply `ABORT` (presumed abort). |
| `ACK` | `COMMITTING`/`ABORTING` | Mark acked. When all acked: log `END`, state → `DONE`. |
| `RETRANSMIT` | `COMMITTING`/`ABORTING` | Resend decision to non-acked participants; reschedule with backoff. |
| `OUTCOME_QUERY` | decided (state or log) | Reply with the decision. |
| `OUTCOME_QUERY` | `COLLECTING` | No reply (participant will retry; the vote timeout will resolve it). |
| `OUTCOME_QUERY` | unknown | Reply `ABORT`. |

### 5.4 Participant state machine (per trade)

States: `PREPARING` (locked, `PREPARED` record being flushed), `PREPARED` (voted yes), `COMMITTED`, `ABORTED`. No state = never seen.

| Event | State | Action |
|---|---|---|
| `PREPARE` | none | Check: not frozen, not expired, `available >= leg amount`. Fail: send `VOTE no(reason)`, state → `ABORTED` (tombstone). Pass: lock, log `PREPARED`, state → `PREPARING`; after flush, state → `PREPARED`, send `VOTE yes`, set `DECISION_WAIT`. |
| `PREPARE` | `PREPARED` | Duplicate: resend `VOTE yes`. |
| `PREPARE` | `ABORTED` | Late or stale: send `VOTE no(ALREADY_ABORTED)`. |
| `COMMIT` | `PREPARED` | Apply leg (debit locked, credit), unlock, log `COMMITTED`, state → `COMMITTED`, send `ACK`. |
| `COMMIT` | `COMMITTED` | Duplicate: send `ACK` again. |
| `COMMIT` | none/`ABORTED`/`PREPARING` | **Invariant violation.** Emit `INVARIANT_VIOLATION` trace event; do not change state. Tests must assert this never happens. |
| `ABORT` | `PREPARED`/`PREPARING` | Release lock, log `ABORTED`, state → `ABORTED`, send `ACK`. |
| `ABORT` | none | Write `ABORTED` tombstone (so a delayed `PREPARE` is refused), send `ACK`. |
| `ABORT` | `ABORTED` | Duplicate: send `ACK`. |
| `DECISION_WAIT` timer | `PREPARED` | Send `OUTCOME_QUERY`; reschedule with exponential backoff. **Never abort unilaterally.** |

---

## 6. Timeouts and recovery

### 6.1 Governing rule
A node may time out and abort on its own **only if it has not made a promise it cannot take back.**
- Participant: voting yes is the promise.
- Coordinator: logging a decision is the promise.

### 6.2 Timer table
| Who | Situation | Timer | On expiry |
|---|---|---|---|
| Coordinator | `PREPARE` sent, votes missing | `vote_timeout_us` (capped at `valid_until_us - now`) | Decide ABORT (reason `VOTE_TIMEOUT`). |
| Coordinator | Decision logged, ACKs missing | `retransmit` (initial → max, doubling) | Resend decision to non-acked nodes. |
| Participant | Voted yes, no decision yet | `decision_wait` (initial → max, doubling) | Send `OUTCOME_QUERY`. Do not abort. |
| Participant | Locked, not yet voted | (none in v1) | `PREPARING` only lasts for the local log flush. A lock lease for a "stalled disk" scenario is a stretch goal. |
| Participant | `PREPARE` for an expired trade | deadline check | Vote no (`EXPIRED`). |

Defaults (all configurable): `vote_timeout_us = 500_000`, `retransmit_initial_us = 1_000_000`, `retransmit_max_us = 8_000_000`, `decision_wait_initial_us = 1_000_000`, `decision_wait_max_us = 8_000_000`, `trade_deadline = 30 s` after proposal.

Sizing guidance for `vote_timeout_us`: at least `2 × max one-way latency (coordinator↔participant) + processing + log flush + jitter margin`. Too short gives **false aborts** on slow links. Too long leaves locks held after a real failure. The simulator should make this trade-off visible (see 11, 12).

### 6.3 Reordering and duplicates
- Jitter and retransmission can reorder or duplicate messages. The state tables above define the response for each case.
- The **tombstone rule** matters: an `ABORT` arriving before its `PREPARE` must leave an `ABORTED` record so the late `PREPARE` is refused.

### 6.4 Crash and recovery model
Each node has **volatile state** (trade states, in-memory ledger, timers) and a **durable log** (survives a crash).

- `on_crash()`: wipe volatile state; discard the node's pending timers; drop messages delivered while the node is down.
- `on_restart()`: rebuild everything from genesis + durable log, then:
  - **Participant:** replay `COMMITTED` records into balances. For each trade with `PREPARED` and no `COMMITTED`/`ABORTED`: restore the lock, state `PREPARED`, immediately send `OUTCOME_QUERY`, schedule `decision_wait`.
  - **Coordinator:** for each `DECISION_COMMIT` (or `DECISION_ABORT`) without `END`: restore state `COMMITTING`/`ABORTING`, resend the decision, schedule `retransmit`. Trades with no decision are forgotten (presumed abort).
- Recovery must be **idempotent**: restarting twice in a row must give the same state.

---

## 7. Fault injection

### 7.1 Crash points
Crash points are defined relative to one `Effects` batch (Section 3.4), which makes them precise and declarative:

```
CrashSpec {
  node, trade_id,
  trigger: { message_type | timer_kind },
  point:   BEFORE_LOG | AFTER_LOG_BEFORE_SEND | AFTER_FIRST_SEND,
  restart_after_us            // or "never"
}
```

- `BEFORE_LOG`: the node crashes while handling; **no log record is written and no message is sent**.
- `AFTER_LOG_BEFORE_SEND`: log is durable; **no message leaves**.
- `AFTER_FIRST_SEND`: the first message in the batch leaves; the rest are lost (relevant for coordinator broadcasts).

### 7.2 Other injectors (scriptable at a sim time)
- `set_link(a, b, latency, jitter, drop)`, `partition(a, b, on/off)`
- `drop_next(message_type, from, to)`
- `node_down(n)` / `node_up(n)`
- `freeze_account(n)`
- `set_param(name, value)` (timeouts, delays)

---

## 8. Scenarios (also the acceptance tests)

Each scenario is a function that builds a config + script, runs to quiescence, and asserts the listed outcomes. Run each with a fixed seed.

| # | Name | Setup | Expected outcome |
|---|---|---|---|
| S1 | Happy path | Normal config | Both nodes `COMMITTED`; balances changed by exactly the trade; coordinator `DONE`. |
| S2 | Buyer lacks funds | Buyer cash < price | Buyer votes no; seller (already locked) receives `ABORT` and **releases its lock**; no balance changes. |
| S3 | Account frozen | `freeze_account(seller)` | Seller votes no (`ACCOUNT_FROZEN`); buyer's lock released. |
| S4 | Slow link, tight timeout | Singapore link latency > `vote_timeout / 2` | Coordinator aborts on timeout; late `VOTE yes` is answered with `ABORT`; both release. **False abort.** Raising the timeout makes the same trade commit. |
| S5 | Participant dead before voting | `node_down(BANK_B)` before `PREPARE` | Coordinator times out, aborts; buyer releases lock. BANK_B on restart sees `ABORT`/tombstone path if it receives it, or presumed abort if it queries. |
| S6 | **In-doubt participant** | Crash BANK_A `AFTER_FIRST_SEND` on `PREPARE` (vote sent), restart later | Coordinator commits; BANK_B commits; BANK_A restarts, finds `PREPARED` with no outcome, sends `OUTCOME_QUERY`, receives `COMMIT`, applies. **Lock-held time spans the outage.** |
| S7 | Crash after vote logged, before sending | Crash BANK_A `AFTER_LOG_BEFORE_SEND` on `PREPARE` | Coordinator never sees a vote, times out, aborts; BANK_A restarts in-doubt, queries, gets `ABORT`. |
| S8 | Crash before logging | Crash BANK_A `BEFORE_LOG` on `PREPARE` | No record and no lock survive; coordinator times out and aborts; BANK_A restarts clean. |
| S9 | Coordinator crash after decision | Crash coordinator `AFTER_LOG_BEFORE_SEND` on the deciding vote | Participants stay in-doubt, retrying `OUTCOME_QUERY`; coordinator restarts, resends `COMMIT`; all commit. **Shows 2PC is blocking.** |
| S10 | Coordinator crash mid-broadcast | Crash coordinator `AFTER_FIRST_SEND` on decision | One participant commits immediately; the other stays in-doubt until the restart resends. |
| S11 | Lost COMMIT | `drop_next(COMMIT, coord, BANK_A)` | Retransmit delivers it; BANK_A commits; duplicates are harmless. |
| S12 | Duplicate/reordered messages | High jitter, duplicated `PREPARE`/`COMMIT` | State tables handle it; no invariant violations. |
| S13 | Lock conflict | Two trades need the same bonds; seller can cover only one | Exactly one commits; the other aborts with `INSUFFICIENT_BONDS`. |
| S14 | Partition | `partition(COORD, BANK_C)` during voting | Timeout abort; after healing, in-doubt participants resolve via query. |

### Global invariants (checked after every scenario, and in a randomized soak test)
1. **Atomicity:** after the system quiesces (all nodes up, network healed), for every trade either both participants are `COMMITTED` or neither is.
2. **Conservation:** total cash and total bonds across all nodes (including locked) are unchanged except by committed trades, which net to zero.
3. **No negative balances; `locked <= total`.**
4. **No leaked locks:** once quiesced, `cash_locked` and `bonds_locked` are zero.
5. **No `INVARIANT_VIOLATION` events.**
6. **Determinism:** same seed ⇒ identical trace (excluding `measured_ns`).
7. **Recovery idempotence:** crash+restart at any quiescent point reproduces identical state.

Add a **randomized soak test**: random trades, random crash specs, random latencies/drops across many seeds; assert invariants 1–5 at the end.

---

## 9. Trace schema

One JSON object per line (JSONL). Everything the UI needs comes from this stream.

```json
{
  "seq": 17,
  "sim_time_us": 120500,
  "trade_id": 3,
  "node": "BANK_A",
  "kind": "MSG_DELIVER",
  "msg_type": "PREPARE",
  "msg_id": 41,
  "from": "COORD",
  "to": "BANK_A",
  "sent_at_us": 120000,
  "deliver_at_us": 120500,
  "simulated_latency_us": 500,
  "log_record": null,
  "state_before": "NONE",
  "state_after": "PREPARING",
  "ledger_before": {"cash_total": 100000000, "cash_locked": 0, "bonds_total": 25000, "bonds_locked": 0},
  "ledger_after":  {"cash_total": 100000000, "cash_locked": 2487500, "bonds_total": 25000, "bonds_locked": 0},
  "handler": "Participant::on_prepare",
  "reason": null,
  "measured_ns": 1840
}
```

`kind` values: `SCRIPT_ACTION`, `MSG_SEND`, `MSG_DELIVER`, `MSG_DROP` (with `reason`: `DROP_RANDOM`, `PARTITION`, `NODE_DOWN`), `LOG_APPEND`, `STATE_CHANGE`, `LOCK`, `UNLOCK`, `APPLY`, `TIMER_SET`, `TIMER_FIRE`, `TIMER_IGNORED`, `CRASH`, `RESTART`, `RECOVERY_STEP`, `REJECTED`, `INVARIANT_VIOLATION`.

Rules:
- `handler` is a static string (`__func__` or an explicit name) so the UI can show **which function ran** for each step. This is the "code mapping" part of the demo.
- `simulated_latency_us` (network and configured delays) and `measured_ns` (real compute time of the handler) are **separate fields and must never be summed or mixed** in any display or metric.
- Trace events are emitted by the simulator core, not by the nodes, so nodes stay pure.

---

## 10. Metrics

Computed from the trace at the end of a run (and incrementally for live views):

- Trades: proposed, committed, aborted (by reason), rejected.
- **Settlement latency** (sim time from `PROPOSE` to last participant `COMMITTED`): mean, p50, p95, p99 computed from the stored per-trade values (do not call a mean a median).
- **Lock-hold time** per participant per trade: `UNLOCK.time − LOCK.time`; summary stats.
- **In-doubt time:** time spent in `PREPARED` before an outcome arrived.
- Timeout aborts vs. vote-no aborts (a timeout abort with no crash or partition is a **false abort**).
- Message counts by type, drops, retransmissions.

---

## 11. Dials (configuration)

| Dial | Default | What it shows |
|---|---|---|
| `seed` | 1 | Reproducibility |
| Per-link latency/jitter/drop | see 3.4 | Region effects |
| `vote_timeout_us` | 500 000 | False-abort rate vs. lock-hold time |
| `retransmit_*`, `decision_wait_*` | see 6.2 | Recovery speed |
| `processing_delay_us` | 100 | Node speed |
| `log_flush_delay_us` | 1 000 | Disk cost of write-ahead logging |
| `trade_deadline` | 30 s | Business expiry |
| Crash specs, partitions, frozen accounts | none | Failure demos |
| Load: trades per simulated second | 1 | Contention, lock conflicts |

### Parameter sweep (CLI)
`--sweep vote_timeout_us=100000:1000000:100000 --sweep link.NY-SG.latency_ms=20:300:20` runs a grid of scenarios and prints a table of abort rate and mean lock-hold time. This is the "press the pedal" demo in text form, and the data source for a UI heatmap later.

---

## 12. Interfaces

### Phase A: CLI (build first)
```
./sim --scenario S6 --seed 42                 # run a scenario, print swimlane + summary
./sim --scenario S6 --trace out.jsonl         # also write JSONL trace
./sim --step                                  # interactive: Enter advances one event
./sim --soak --runs 2000                      # randomized invariant testing
./sim --sweep ...                             # parameter sweep
```

Text swimlane example (one row per event, time in sim ms):

```
t(ms)   BANK_A            COORD              BANK_B
0.0                        PROPOSE #1
0.1                        ──PREPARE──────────────────────►
0.1     ◄──PREPARE─────────
0.6     lock cash; log PREPARED
1.6     ──VOTE yes────────►
...
```

### Phase B: HTTP + SSE bridge (after the core is solid)
- `GET /api/stream` (SSE): trace events. Give each SSE client a **bounded queue** so a slow client cannot stall the simulator.
- `POST /api/scenario`, `POST /api/step`, `POST /api/run`, `POST /api/pause`, `POST /api/config` (dials), `POST /api/fault`.
- `GET /api/trace/:trade_id`: per-trade waterfall.
- `GET /api/nodes`: current state per node (balances, locks, trade states, log tail).
- **Threading:** the simulator runs on one thread; HTTP handlers enqueue commands to it through a mutex-protected command queue. Never call into the simulator from HTTP threads.
- **Rewind** = re-run from the seed to event N (cheap because runs are deterministic).
- Playback speed is just pacing of event emission versus sim time; it does not change results.

### Phase C: Frontend
Swimlane view (lane per node, time downward, arrows labeled with message type and simulated latency), per-node panels (available/locked balances, trade states, log tail), per-trade waterfall, dial controls, scenario picker, step/run/pause/rewind, live metrics. The **handler** field from the trace is shown next to each step so a viewer can see the function responsible.

---

## 13. Code structure (suggested)

```
glassbox-dvp-sim/
├── CMakeLists.txt
├── README.md                 # includes Section 17 (simplifications)
├── DESIGN.md                 # this file
├── include/ and src/ (mirror each other)
│   ├── sim/        simulator, event_queue, network, rng, clock/types
│   ├── domain/     trade, local_ledger
│   ├── storage/    durable_log          # in-memory vector of records, optional file backing
│   ├── protocol/   messages, log_records, coordinator, participant
│   ├── faults/     crash_spec, injectors
│   ├── trace/      trace_event, jsonl_writer, metrics, text_swimlane
│   ├── scenarios/  s01_... s14_..., scenario registry
│   └── main.cpp    CLI
├── tests/          one test per scenario + invariants + determinism + soak
└── frontend/       (Phase C)
```

### From the existing prototype
**Reuse (adapt):**
- `types.hpp`: stage/status enums as inspiration for reason codes; fixed-size ids are fine but not required.
- `AccountLedger` validation logic and reason strings → becomes `LocalLedger` with the added `*_locked` fields.
- `StageTelemetry` → evolves into `TraceEvent` (Section 9).
- `BinaryWAL` → `DurableLog`. Keep the record-append and replay ideas; add record types, and a simulated durability boundary (records appended but not yet "flushed" are lost on `BEFORE_LOG` crash).
- Terminal color renderer in `main.cpp` → starting point for `text_swimlane`.

**Drop / replace:**
- `SPSCRingBuffer`, `alignas(64)`, mutexes, the 5-station pipeline, `process_direct`, and the throughput benchmark: they belong to a single-node latency story, not this one.
- Any claim of "zero-copy", "single memory fence", "mmap append", "32-byte receipts", "median latency" unless made true and measured.
- The "weekend repo" demo: stretch goal only; a real version needs a second scheduled reverse leg with interest, so implement it as a **new trade** created by a timer, not as a special case.
- Company-specific names in identifiers and UI.

Use `std::chrono::steady_clock` (not `high_resolution_clock`) for `measured_ns`.

---

## 14. Testing strategy

- **Unit tests:** event queue ordering (ties broken by `seq`), network latency/jitter bounds, ledger lock/unlock arithmetic, log replay.
- **Scenario tests:** S1–S14, each asserting the invariants in Section 8.
- **Property/soak test:** thousands of seeded random runs with random crash specs; assert atomicity, conservation, no leaked locks.
- **Determinism test:** run each scenario twice, diff traces (excluding `measured_ns`).
- **Recovery idempotence test:** restart twice, compare state.
- Compile with `-Wall -Wextra -Wpedantic` and run tests under ASan/UBSan.

---

## 15. Milestones

| M | Deliverable | Done when |
|---|---|---|
| M0 | Rename project; remove vendor names; delete ring buffer/pipeline/bench; keep reusable pieces | Builds; README updated with framing |
| M1 | Simulator core: virtual clock, event queue, RNG, network model | Ordering + determinism + latency-bound tests pass |
| M2 | `LocalLedger` with locks; `DurableLog` with replay | Unit tests pass; balances rebuild from log |
| M3 | Coordinator + participants, happy path (S1) | S1 passes with all invariants |
| M4 | Vote-no and timeout aborts (S2–S5, S13) | Scenarios pass; no leaked locks |
| M5 | Crash points, recovery, retransmit (S6–S12, S14) | Scenarios pass; recovery idempotence test passes |
| M6 | Trace JSONL, metrics, text swimlane, `--step` | Output matches Section 9 schema; metrics correct |
| M7 | Soak test + parameter sweep | 2000-run soak passes; sweep table prints |
| M8 | HTTP/SSE bridge | UI-less client can stream and step a scenario |
| M9 | Frontend (swimlane, panels, dials) | Demo of S1, S4, S6, S9 end to end |

Cut order if time is short: M9 polish → M8 → sweep. Keep M1–M7; they are the substance.

---

## 16. Conventions for the implementer

- C++20, no threads inside the simulation core, no wall-clock or global state in node logic.
- All randomness through the single seeded RNG; iterate containers in a deterministic order (use ordered maps or sorted ids wherever order affects behavior).
- Money and quantities are integers.
- Nodes return `Effects`; only the simulator performs I/O (logging, sending, timers).
- Never write a log record after the corresponding message in the same step; the simulator's structure (Section 3.4) already prevents it. Do not bypass it.
- When behavior is ambiguous, prefer the safe interpretation (abort over commit; query over guess), add a note in the trace, and list it under Open Questions below.

---

## 17. Simplifications compared with real systems (put in README)

- Single coordinator (also the sequencer): it is a blocking point and a single point of failure. Real systems replicate or separate these roles.
- Two-phase commit semantics only; no consensus protocol, no Byzantine faults, no cryptographic signatures or identity.
- No privacy model: every participant sees what the protocol sends them, nothing more, but there is no sub-transaction confidentiality.
- One asset, whole-trade settlement, no netting or partial fills.
- Simulated disk and network; real fsync cost, TCP behavior, clock skew and GC pauses are not modeled.
- Latency numbers are illustrative defaults, not measurements of any network.

---

## 18. Open questions

1. Should the coordinator support multiple concurrent trades per pair beyond lock conflicts (queueing/priorities)? Default: no, v1 handles concurrency only through locks.
2. Should `PREPARE` be retransmitted by the coordinator before the vote timeout? Default: no.
3. Add a participant lock lease and "stalled disk" scenario? Stretch.
4. Add a replicated coordinator scenario to show how blocking is reduced? Stretch.
5. Verify terminology and any claims against Canton's public documentation before presenting the project externally.

---

## 19. Glossary

- **DvP:** delivery versus payment; asset and cash move together or not at all.
- **Atomic settlement:** both legs happen, or neither does.
- **Prepare / vote / commit / abort:** the phases of two-phase commit.
- **In-doubt:** a participant that voted yes and has not yet learned the outcome; it must hold its lock and ask.
- **Presumed abort:** with no commit decision on record, the outcome is abort.
- **Write-ahead log (WAL):** record intent durably before acting on it, so a restart can recover.
- **Tombstone:** a record that a trade was aborted, so late messages for it are refused.
- **Quiescence:** no events pending, all nodes up, network healed.
