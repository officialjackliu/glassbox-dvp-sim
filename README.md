# Glass-Box DvP Simulator

A local, deterministic simulation of atomic Delivery-versus-Payment (DvP) settlement across a distributed ledger. Every message, lock, log write, timeout, crash, and recovery step is visible and mapped to the code that produced it.

This is an independent, educational, open-source simulator. It is not an implementation of any vendor's system.

## Development status

The repository is initialized for the incremental implementation described in `DESIGN.md`. The current executable and test targets are smoke-test scaffolding only.

## Requirements

- C++20
- CMake 3.20 or newer
- Clang or GNU C++

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Sanitizers

```sh
cmake -S . -B build-sanitizers \
  -DGLASSBOX_ENABLE_ASAN=ON \
  -DGLASSBOX_ENABLE_UBSAN=ON
cmake --build build-sanitizers
ctest --test-dir build-sanitizers --output-on-failure
```

## Simplifications

- A single coordinator also acts as the sequencer.
- Two-phase commit semantics only; no consensus protocol, Byzantine fault tolerance, cryptographic signatures, or identity system.
- No privacy model or sub-transaction confidentiality.
- One asset, whole-trade settlement, no netting or partial fills.
- Simulated disk and network behavior; real fsync cost, TCP behavior, clock skew, and garbage collection pauses are not modeled.
- Default latency numbers are illustrative placeholders, not measurements of a real network.
