# Glass-Box DvP Simulator

A deterministic simulator of atomic **delivery versus payment** (DvP) settlement
between banks, built as a "glass box": every message, lock, disk write, timeout,
crash and recovery is visible, explained in plain English, and tied to the C++
function that caused it.

It is an independent, educational, open-source model, not an implementation of
any vendor's system, and its numbers are illustrative (see
[Simplifications](#simplifications)).

## Run it

```sh
make            # build, then open the how-to-play menu: pick a scenario, watch, repeat
make tour       # guided demo of five scenarios
make manual     # every command and flag
```

The menu, the keys, all 14 scenarios and every setting are explained in
**[docs/MANUAL.md](docs/MANUAL.md)**.

## What the simulation models

**Delivery versus payment.** In a bond trade, the buyer pays cash and the seller
delivers bonds. DvP means the two legs settle together or not at all: nobody
ends up with the cash *and* the bonds, and nobody gives one without receiving the
other.

The hard part is that the two legs live in different places. Here each bank keeps
its own ledger on its own node (New York, London and Singapore), and the nodes can
talk only over a network that delays, loses and duplicates messages. Any node can
also crash.

**Two-phase commit (2PC) with presumed abort.** This is the protocol that makes
the trade atomic anyway:

1. **Submit:** a client sends the trade ticket to the coordinator. The
   coordinator validates it and writes `PROPOSED` to its disk.
2. **Lock (prepare):** the coordinator asks both banks to prepare. Each bank
   checks the trade (account not frozen, deadline not passed, enough *available*
   cash or bonds) and locks its leg, so the same money cannot be promised twice.
3. **Vote:** a bank votes YES only after its `PREPARED` record is on disk. From
   then on it may not back out on its own. A bank that cannot settle votes NO.
4. **Decide:** with two YES votes the coordinator writes `DECISION_COMMIT`. That
   write is the commit point: from now on the trade *will* settle, even if the
   coordinator crashes. A NO vote, or no answer before the vote timeout, means
   ABORT.
5. **Settle:** each bank applies its leg (pay and receive bonds, or deliver and
   receive cash) or releases its lock, then acknowledges.
6. **Confirm:** with both acknowledgements the coordinator writes `END`. Until
   then it keeps resending the decision.

A bank that voted YES and hears nothing is **in doubt**. It keeps its lock and
asks the coordinator; it may not decide alone. This is why 2PC is called
blocking, and scenario S9 shows it. If the coordinator has no record of a trade,
the answer is ABORT ("presumed abort").

Scenario S1, exactly as the simulator runs it (simulated time; latencies are
1 ms within New York and 35 ms to London):

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ time ms   BANK_A (New York)         COORD (New York)       BANK_B (London)   │
│     0.0                             PROPOSE: validate, open #1               │
│     1.0                             disk: PROPOSED                           │
│     1.1           ◄──── PREPARE ────┤├──── PREPARE ────►                     │
│     2.1   check, lock $24,875.00                                             │
│     3.1   disk: PREPARED                                                     │
│     3.2   ──── VOTE YES ────►                                                │
│     4.2                             YES from A (1 of 2)                      │
│    36.1                                                    check, lock 25    │
│    37.1                                                    disk: PREPARED    │
│    37.2                                    ◄──── VOTE YES ────               │
│    72.2                             YES from B: decide COMMIT                │
│    73.2                             disk: DECISION_COMMIT  (commit point)    │
│    73.3           ◄──── COMMIT ─────┤├──── COMMIT ─────►                     │
│    74.3   pay $24,875.00, +25 bonds                                          │
│    75.3   disk: COMMITTED                                                    │
│    75.4   ──── ACK ────►                                                     │
│    76.4                             ACK from A (1 of 2)                      │
│   108.3                                                    -25 bonds,        │
│                                                            +$24,875.00       │
│   109.3                                                    disk: COMMITTED   │
│   109.4                                    ◄──── ACK ─────                   │
│   144.4                             ACK from B: DONE                         │
│   145.4                             disk: END                                │
└──────────────────────────────────────────────────────────────────────────────┘
```

## System design

Five layers. Each layer uses only the ones below it, and the nodes never see the
network, the clock or each other.

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ FRONT END: the terminal player       src/main.cpp, include/cli/              │
│   make ─► menu ─► Player ─► draw(), draw_intro(), draw_summary()             │
│                      │        narrate(): each event in plain English         │
│                      │        pace(): slow motion, without touching the run  │
│                      └─► record(): steps the run one event at a time and     │
│                          copies every node's view (read-only)                │
├──────────────────────────────────────────────────────────────────────────────┤
│ SCENARIOS                            include/scenarios/                      │
│   make_s1_happy_path() ... make_s14_partition()  ─► ScenarioSetup (data)     │
│   ScenarioRun: builds the simulator + nodes, runs the script, keeps trace    │
│   check_invariants(): conservation, atomicity, locks, after every event      │
├──────────────────────────────────────────────────────────────────────────────┤
│ SIMULATOR CORE                       include/sim/, src/simulator.cpp         │
│   Simulator::step() ─ EventQueue (time, seq) ─ VirtualClock (simulated us)   │
│     deliver / timer ─► node handler ─► Effects ─► apply_effects ─► events    │
│   NetworkModel: latency, jitter, drops, partitions ◄── DeterministicRng      │
│   DurableLog per node (survives crashes) · epochs · crash points (faults/)   │
│   TraceSink: one TraceRecord per step, naming the function that ran          │
├──────────────────────────────────────────────────────────────────────────────┤
│ NODES: pure state machines           include/protocol/                       │
│   message or timer in ──► Effects { logs, sends, timers, notes } out         │
│   Coordinator: 2PC with presumed abort   Participant x3: BANK_A, B, C        │
├──────────────────────────────────────────────────────────────────────────────┤
│ DOMAIN                               include/domain/                         │
│   LocalLedger (balances, locks) · Trade (validated ticket) · LogRecord       │
│   replay_participant() / replay_coordinator(): rebuild state from disk       │
├──────────────────────────────────────────────────────────────────────────────┤
│ CORE TYPES   SimTimeUs, Cents, Units, TradeId: int64, checked arithmetic     │
└──────────────────────────────────────────────────────────────────────────────┘
```

### The event loop

The simulator is a **single-threaded discrete-event loop**:

1. Pop the earliest event and jump the clock to its time. There is no waiting
   and no wall clock.
2. Hand the event to the node it belongs to.
3. Turn the node's reply into future events.

Nodes are **pure state machines**: each handler takes one message or timer plus
the current simulated time, and returns an `Effects` batch with log records to
write, messages to send, timers to set, and trace notes. The simulator alone
decides when each of those happens, which enforces the **write-ahead rule** in
one place:

- the batch's log records become durable after `log_flush_delay_us` (1 ms);
- its messages leave `processing_delay_us` (0.1 ms) later, so no promise is
  announced before it is on disk;
- its timers start when the messages leave.

Here is one event, BANK_A receiving PREPARE in S1, followed through the code:

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ Simulator::step()                                                            │
│  └ VirtualClock::pop_next()         time jumps to the next event: 2.1 ms     │
│    └ dispatch_deliver() ─► deliver_to_node(BANK_A, PREPARE)                  │
│      └ Participant::on_message() ─► on_prepare()                             │
│          frozen? expired? available cash >= $24,875.00?    all ✓             │
│          apply(ledger, LockCash{$24,875.00})               memory only       │
│          return Effects{ logs:   Prepared{trade, BUYER}                      │
│                          sends:  VOTE YES ─► COORD                           │
│                          timers: DECISION_WAIT, 1 s                          │
│                          notes:  LOCK, STATE_CHANGE }                        │
│  └ apply_effects_impl(BANK_A, effects, now = 2.1 ms)                         │
│      flush_log    at 2.1 + 1.0       = 3.1 ms ─► DurableLog::append()        │
│                                          ─► Participant::on_log_durable()    │
│      release_send at 2.1 + 1.0 + 0.1 = 3.2 ms ─► NetworkModel::              │
│                                          schedule_delivery(): one jitter     │
│                                          draw, one drop draw ─► 4.2 ms       │
│      fire_timer   at 3.2 + 1,000.0   = 1,003.2 ms (ignored if settled)       │
│      notes ─► TraceSink, tagged "Participant::on_prepare"                    │
└──────────────────────────────────────────────────────────────────────────────┘
```

Timers are never cancelled. When one fires, the simulator ignores it if the node
is down, if it was set before the node's last crash (an older *epoch*), or if the
trade has moved on (`Node::trade_state()` no longer matches).

### Crash and recovery

A crash wipes a node's memory and keeps its disk. A **crash point** decides how
much of the batch being handled survives: nothing (`BEFORE_LOG`), the disk write
only (`AFTER_LOG_BEFORE_SEND`), or the disk write and the first message
(`AFTER_FIRST_SEND`). Restart is replay. Scenario S6:

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ BANK_A at 3.2 ms        memory                        disk                   │
│   before the crash      #1 PREPARED, $24,875 locked   [1] PREPARED #1        │
│   CRASH (epoch 0 → 1)   wiped                         kept                   │
│                         queued flushes, sends and timers of epoch 0 are      │
│                         ignored when they come due                           │
│   RESTART at 5.0 s      replay_participant(genesis, disk):                   │
│                         $24,875 locked again, #1 IN DOUBT                    │
│                         ─► OUTCOME_QUERY ─► COORD answers COMMIT ─► settle   │
└──────────────────────────────────────────────────────────────────────────────┘
```

### Determinism and the glass box

- **One seeded random generator** (`DeterministicRng`) makes all random choices.
  Every message takes exactly one jitter draw and one drop draw, in a fixed
  order. Events at the same time run in the order they were scheduled. So the
  same scenario and seed always produce the same trace; a test checks this.
- **The trace:** every step emits one `TraceRecord` naming what happened and
  which function did it. The nodes add notes (`LOCK`, `APPLY`,
  `STATE_CHANGE`, ...) but never write traces themselves.
- **The player** never changes the run. `record()` steps the same `ScenarioRun`
  one event at a time and copies each node's view after every event, and a test
  checks that the recorded trace equals a plain run's. Playback speed only
  changes how long things stay on screen.

## File structure

```
glassbox-dvp-sim/
├── Makefile               make: menu · tour · watch · story · test · help
├── CMakeLists.txt         two libraries (glassbox_core, glassbox_cli), the
│                          executable, 40 registered tests
├── DESIGN.md              the design: protocol, records, scenarios, trace
├── docs/MANUAL.md         user manual
├── include/               headers, one comment block per type and function
│   ├── sim/               simulator core
│   │   ├── core_types     SimTimeUs, Cents, Units, TradeId, checked arithmetic
│   │   ├── event_queue    events and the (time, seq) min-heap
│   │   ├── virtual_clock  simulated time; real time only for display
│   │   ├── deterministic_rng   the one seeded RNG
│   │   ├── network_model  links, latency, jitter, drops, partitions
│   │   ├── node, effects  the pure node interface and its Effects batch
│   │   ├── simulator      the event loop, write-ahead rule, crashes, epochs
│   │   ├── timers         exponential backoff
│   │   └── trace          TraceRecord and TraceSink
│   ├── domain/            ledgers and durability, no simulator knowledge
│   │   ├── local_ledger   balances, locks, frozen; every change via apply()
│   │   ├── trade          ticket validation and trade ids
│   │   ├── log_records    what nodes write to disk
│   │   ├── durable_log    append-only log with sequence numbers
│   │   └── replay         rebuild a node's state from its log
│   ├── protocol/          messages, coordinator, participant (2PC)
│   ├── faults/            crash points
│   ├── scenarios/         S1-S14, ScenarioRun, the invariant checker
│   ├── trace/             one-line trace format
│   └── cli/               front end: recording, narrator, pacing, screen,
│                          guide, dials (settings), summary, terminal
├── src/                   implementations, same layout; main.cpp = menu + flags
└── tests/                 33 test programs (one per module, scenario group and
                           front-end part), plus 7 command-line checks
```

The `domain/` and `protocol/` headers may not include simulator internals. A
test (`node_purity_test`) checks this.

## Why C++

The point of the project is exact, explainable behavior, and C++ gives the most
control over it:

- **Determinism.** There is no garbage collector and there are no hidden
  threads; one thread runs one event loop. Every source of variation (time,
  randomness, ordering) is explicit, so a run is reproducible down to the byte.
- **Exact money and time.** Amounts are `int64_t` cents and quantities, time is
  `int64_t` microseconds, and anything that could overflow goes through
  `checked_add_i64` / `checked_sub_i64` / `checked_multiply_i64`, which report
  overflow instead of wrapping. There is no floating point anywhere, and
  `static_assert`s and tests enforce it.
- **Invalid states don't compile.** `TradeId`, `Cents` and `Units` are distinct
  types, so the compiler rejects mixing them. A `Trade` can only be created by
  `validate_and_assign()`, so holding one proves it passed validation.
  Messages, log records, events and ledger operations are `std::variant`s of
  plain structs, and enum switches are checked by `-Wall -Werror`.
- **Value semantics.** Nodes are pure functions over plain values, so the player
  can copy every node's state after every event, and replay can rebuild a node
  from its log with the same code that runs live (`apply()` on a `LedgerOp`).
- **Tooling.** It builds warning-free with `-Wall -Wextra -Wpedantic -Werror` in
  Debug, Release and UndefinedBehaviorSanitizer builds (AddressSanitizer is
  supported where the toolchain runs it).

C++ is also common in trading and settlement infrastructure, where predictable
behavior and control over memory matter. This model uses it for that control,
not for speed: it makes no performance claims.

## How we know it is right

- **Scenario tests** pin every scenario's exact timeline (each message, lock,
  disk write and decision at its simulated microsecond) and its final balances.
- **Invariants after every single event** in every scenario:
  - money and bonds are conserved, locked amounts included;
  - no balance is negative;
  - locks match the open trades;
  - no trade is COMMITTED at one bank and ABORTED at the other;
  - once the run is over, both banks agree on every trade and no lock is left.
- **Recovery:** crashing and restarting every node at the end of a run, once
  and twice, gives the same state. Random seeds and duplicated or reordered messages are swept over
  hundreds of runs.
- **Mutation testing:** bugs were planted one line at a time (a flipped
  comparison, a skipped write, swapped arguments) to confirm a test fails for
  each. The few that survived were either equivalent to the original code or
  unreachable from any scenario; each is noted in the commit messages.

```sh
make test        # Debug
make test-all    # Debug, Release and UBSan: what must pass before a merge
```

Without the Makefile:

```sh
cmake -S . -B build -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Requirements: C++20, CMake 3.20 or newer, Clang or GCC.

## Simplifications

- A single coordinator, which also sequences trades: a blocking point and a
  single point of failure. Real systems replicate or separate these roles.
- Two-phase commit only: no consensus protocol, no Byzantine faults, no
  cryptographic signatures or identities.
- No privacy model: each bank sees only what the protocol sends it, but there is
  no sub-transaction confidentiality.
- One asset and whole-trade settlement: no netting and no partial fills.
- Simulated disk and network: real fsync cost, TCP behavior, clock skew and
  garbage-collection pauses are not modeled.
- Latencies (1 ms within New York, 35 ms to London, 120 ms to Singapore) are
  illustrative defaults, not measurements of any network.
