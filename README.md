# Glass-Box DvP Simulator

A local, deterministic simulation of atomic Delivery-versus-Payment (DvP) settlement across a distributed ledger. Every message, lock, log write, timeout, crash, and recovery step is visible and mapped to the code that produced it.

This is an independent, educational, open-source simulator. It is not an implementation of any vendor's system.

## See it run

```sh
make tour                 # guided demo: S1, S2, S6, S9, S14 in slow motion
make watch S=S9           # one scenario (SPEED=slow|normal|fast)
make watch S=S1 SET="vote_timeout=30ms"   # change a setting, watch the outcome change
make story S=S6           # the same explanation as plain text
make list                 # all scenarios
```

The player runs the real simulation first, then replays it event by event:
each node's balances, locks, votes, timers and disk; every message crossing
the network; a plain-English explanation of each step with the function that
ran; and the safety checks after every event. Playback speed only changes how
fast events are shown, never the result. Keys: space pause, n next event,
+/- speed, r restart, q quit.

## Development status

The repository is being implemented incrementally from `DESIGN.md`. Each
stable build chunk is isolated on a short-lived feature branch and merged
through its tests and code review.

## Source organization

- `include/sim/core_types.hpp` defines immutable IDs, virtual time, money,
  quantities, node identifiers, reason codes, outcomes, and overflow helpers.
- `include/sim/event_queue.hpp` owns deterministic `(time_us, seq)` event
  ordering. The simulator and scenario code call `schedule()`, then
  `pop_next()` through `VirtualClock`.
- `include/sim/virtual_clock.hpp` advances only as events are popped. It also
  wraps `std::chrono::steady_clock` for measured handler nanoseconds;
  `measured_ns` is never used for simulation decisions.
- `include/sim/deterministic_rng.hpp` owns scenario random state and centralizes
  jitter, PPM drop, and network draw behavior.
- `include/sim/network_model.hpp` owns directed links, partitions, node-down
  state, next-message drops, and random delivery scheduling. Default regional
  values are illustrative placeholders and every directed link is configurable.
  Node-down inbound state is checked when scheduling and must be rechecked
  by the future simulator loop when a deliver event is popped.
- `include/sim/effects.hpp` and `include/sim/node.hpp` define the pure node
  interface and Effects batch. Nodes receive input and virtual time only and
  return logs, sends, timers, and trace notes.
- `include/sim/simulator.hpp` and `src/simulator.cpp` provide the single-threaded
  event loop, epoch invalidation, deferred log/send timers, node registry,
  scripted actions, and minimal trace sink.
- `include/sim/timers.hpp` provides stateless, overflow-safe timer backoff helpers.
  Timer requests carry a relative delay, attempt, trade ID, and expected trade state.
- `src/main.cpp` is the integration smoke executable. It constructs core values,
  schedules two events, steps them through virtual time, and emits a synthetic
  trace line.

Source headers explain public responsibility and typical callers. Implementation
comments are intentionally limited to ordering, ownership, overflow, and
determinism rules that are not obvious from the code.

## Requirements

- C++20
- CMake 3.20 or newer
- Clang or GNU C++

## Build

```sh
cmake -S . -B build \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++
cmake --build build
ctest --test-dir build --output-on-failure
```

`CMakeLists.txt` creates one executable per source integration test and registers
each test with CTest. `-Wall -Wextra -Wpedantic` is enabled for all targets.

## Tests

The tests currently cover core values, event ordering, virtual timing, and
seeded RNG behavior.

Run the full suite:

```sh
ctest --test-dir build --output-on-failure
```

Run one component test:

```sh
ctest --test-dir build -R deterministic_rng_test --output-on-failure
ctest --test-dir build -R event_queue_test --output-on-failure
ctest --test-dir build -R virtual_clock_test --output-on-failure
```

Run the integration executable:

```sh
./build/glassbox-dvp-sim
```

## Sanitizers

```sh
cmake -S . -B build-ubsan \
  -DGLASSBOX_ENABLE_UBSAN=ON
cmake --build build-ubsan
ctest --test-dir build-ubsan --output-on-failure
```

## Simplifications

- A single coordinator also acts as the sequencer.
- Two-phase commit semantics only; no consensus protocol, Byzantine fault tolerance, cryptographic signatures, or identity system.
- No privacy model or sub-transaction confidentiality.
- One asset, whole-trade settlement, no netting or partial fills.
- Simulated disk and network behavior; real fsync cost, TCP behavior, clock skew, and garbage collection pauses are not modeled.
- Default latency numbers are illustrative placeholders, not measurements of a real network.
