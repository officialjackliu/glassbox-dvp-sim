# glassbox-dvp-sim manual

glassbox-dvp-sim shows how an atomic delivery-versus-payment (DvP) trade settles
between banks. A buyer pays cash and a seller delivers bonds, and either both
happen or neither does, even when a bank crashes or the network fails.

Every message, lock, disk write and timer is shown, together with the line of
code that caused it. It is a simplified educational model, not a real system
(see "Simplifications" in the README).

## 1. Start here

```sh
cd glassbox-dvp-sim
make            # builds the simulator, then opens the how-to-play menu
```

Type `1` and press Enter to watch the happy path. When a scenario ends, or when
you press `q` while it plays, you are back at the menu. Pick another one, or type
`q` to leave.

For a guided demo of five scenarios in a row, run `make tour`.

You need a terminal at least 80 columns wide; 100 or more looks best. The first
`make` takes a minute to compile.

## 2. The menu

| Type | What it does |
|---|---|
| `1` … `14` (or `S6`, `watch 6`) | Watch that scenario in slow motion |
| `r 6` (or `read 6`) | Read scenario 6 as text instead of watching it |
| `t` | The guided tour: scenarios 1, 2, 6, 9, 14 |
| `speed fast` | Playback speed: `slow`, `normal`, `fast`, or `25` … `800` (%) |
| `seed 7` | Another random seed (different random network delays and losses) |
| `set vote_timeout=30ms` | Change the simulated system (see section 5) |
| `clear` | Remove all settings |
| `settings` | List every setting you can change |
| `keys` | The keys you can use while watching |
| `help` | Every command-line flag |
| `q` | Quit |

The line `Now: speed … · seed … · settings: …` shows what applies to the next
scenario you watch. Speed, seed and settings stay set until you change them.

## 3. While watching

Each scenario opens with a card that says what will happen, which banks take
part and with how much money, and what to watch for. Press `n` or Enter to start
(it also starts by itself after about 12 seconds at normal speed).

| Key | Action |
|---|---|
| space | Pause / resume |
| `n`, Enter, → | Next event. While paused, each press steps one event |
| `+` / `-` (↑ / ↓) | Faster / slower, from 25% to 800% |
| `r` | Restart this scenario |
| `q`, Esc, Ctrl-C | Stop and go back to the menu |

At normal speed, 1 ms of simulated time takes 40 ms on screen, so a message from
New York to London (35 ms) takes 1.4 s to cross. Each event stays on screen for
about 1.8 s. Long quiet waits are shortened, with a notice. The speed only
changes how fast events are shown; the result is the same at every speed.

### The screen, top to bottom

```
t = 3.600 ms                                                     ▶ playing · speed 100% · event 7/28

#1  BANK_A buys 25 UST 10Y bonds from BANK_B for $24,875.00
    ✓ submit ─ ✓ lock ─ ● vote ─ ○ decide ─ ○ settle ─ ○ confirm

┌─ COORD · New York ───────────┐ ┌─ BANK_A · New York ──────────┐ ┌─ BANK_B · London ────────────┐
│ #1 COLLECTING                │ │ cash             $975,125.00 │ │ cash           $1,000,000.00 │
│   votes A ·  B ·             │ │   locked          $24,875.00 │ │ bonds                 25,000 │
│ timer vote @501.100 ms       │ │ bonds                 25,000 │ │ disk  (empty)                │
│ disk                         │ │ #1 PREPARED (voted YES)      │ │                              │
│   PROPOSED t1                │ │ timer ask @1,003.200 ms      │ │                              │
│                              │ │ disk                         │ │                              │
│                              │ │   PREPARED t1                │ │                              │
└──────────────────────────────┘ └──────────────────────────────┘ └──────────────────────────────┘

On the wire   · next event in 600 µs
  PREPARE     COORD ──●───────────────────────────────▶ BANK_B   35.0 ms  New York → London
  VOTE YES   BANK_A ─────────────●────────────────────▶ COORD     1.0 ms  within New York

▶ BANK_A sends its YES vote to COORD
   · its PREPARED record is already on disk, so the promise survives a crash
   · travels 1.0 ms, within New York
   code Effects::release ← Participant::on_prepare

✓ money conserved   ✓ no negative balances   ✓ locks match open trades
```

- **Clock and status:** the simulated time, play or pause, the speed, and how
  many events have happened so far.
- **Trade ticket and stage bar:** each trade moves through six stages:
  - *submit*: the coordinator checks and records the ticket;
  - *lock*: each bank checks and sets aside its side;
  - *vote*: YES or NO;
  - *decide*: the coordinator writes COMMIT or ABORT to disk;
  - *settle*: banks pay and deliver, or release their locks;
  - *confirm*: the banks acknowledge, and the coordinator writes END.

  `✓` means the stage is done and `●` is the current stage. At the end the bar
  says SETTLED or CANCELLED.
- **Node panels:** one per node taking part.
  - Each bank shows its free and locked cash and bonds, its state for each
    trade, its timers, and what is on its disk.
  - The coordinator shows the votes and acknowledgements it has received, and
    whether its decision is on disk.
  - A crashed node shows `✗ DOWN: memory lost`: its memory is gone, but its
    disk is kept.
- **On the wire:** every message in transit. The dot moves at the speed of its
  real latency.
- **The event:** what just happened in plain English, the checks or amounts
  behind it, `why` (the rule that made it happen), and `code` (the function that
  ran).
- **Safety checks:** these run after every event. A red `✗` would mean the
  simulator found a bug.

At the end, a card shows:
- each trade's outcome;
- every bank's balances, start and end;
- how long assets were locked;
- the longest time a bank waited "in doubt";
- how many messages were sent, lost and resent.

## 4. The scenarios

The cities are illustrative: COORD and BANK_A are in New York, BANK_B in London,
BANK_C in Singapore. Each bank starts with $1,000,000.00 and 25,000 bonds. The
standard trade is 25 bonds for $24,875.00.

| # | Scenario | What happens |
|---|---|---|
| 1 | Happy path | Both banks lock and vote YES, and both legs settle together |
| 2 | Buyer lacks funds | BANK_A is one cent short and votes NO; BANK_B releases its bonds |
| 3 | Seller's account is frozen | BANK_B votes NO; BANK_A releases its cash |
| 4 | Slow link, tight timeout | The vote from Singapore arrives too late, so a trade both banks agreed to is aborted (a false abort) |
| 5 | A bank is offline | BANK_B misses PREPARE and the vote timer runs out; when BANK_B returns, a resent ABORT reaches it |
| 6 | Bank crashes after voting YES | BANK_A is down for 5 s, then restores its lock from disk, asks, and settles |
| 7 | Bank crashes before its vote leaves | The promise is on disk but was never sent: abort, then recovery by asking |
| 8 | Bank crashes before writing anything | BANK_A remembers nothing; a resent ABORT closes the trade |
| 9 | Coordinator crashes after deciding | Both banks wait 5 s holding their locks (2PC blocks), then the restarted coordinator resends COMMIT |
| 10 | Coordinator crashes mid-announcement | BANK_A settles at once; BANK_B waits until the restart |
| 11 | A COMMIT is lost | The coordinator resends after 1 s |
| 12 | Duplicated and reordered messages | Copies and overtaking messages never lock or settle twice |
| 13 | Two trades, one set of bonds | The first PREPARE wins BANK_B's bonds; the second trade is refused |
| 14 | Network partition | The link to BANK_C is cut, so the vote is lost and the trade aborts; after the link heals, BANK_C asks and learns ABORT |

## 5. Settings: change the system and watch it react

Settings change the simulated system before a scenario runs. At the menu, type
`set KEY=VALUE`; on the command line, add `--set KEY=VALUE` (repeatable).

| Setting | Meaning |
|---|---|
| `vote_timeout=30ms` | How long the coordinator waits for both votes before aborting (default 500 ms) |
| `latency.COORD-BANK_B=80ms` | One-way delay of a link, both directions |
| `drop.COORD-BANK_B=20%` | Share of messages a link loses, 0 to 100% |
| `restart_after=2s` | How long a crashed node stays down (scenarios 6 to 10) |

- **Nodes:** `COORD`, `BANK_A`, `BANK_B`, `BANK_C`.
- **Time units:** whole numbers with `us`, `ms` or `s`, from 1 µs to 1 hour.
- **Drop rate:** a whole number with `%`.

A bad setting is rejected and nothing changes. Link changes show up in the
scenario as narrated network events.

Try these:
- `set vote_timeout=30ms`, then `1`: the happy path aborts, because the PREPARE
  needs 35 ms just to reach London.
- `set vote_timeout=900ms`, then `4`: the false abort becomes a commit.
- `set restart_after=200ms`, then `9`: the banks are blocked for much less time.

## 6. Command line

| Command | What it does |
|---|---|
| `glassbox-dvp-sim` | The menu (in a terminal; otherwise prints the manual) |
| `glassbox-dvp-sim --menu` | The menu, even without a terminal |
| `glassbox-dvp-sim --tour` | Scenarios 1, 2, 6, 9, 14 one after another |
| `glassbox-dvp-sim --scenario S6 --watch` | Watch one scenario in slow motion |
| `glassbox-dvp-sim --scenario S6 --story` | The same explanation as plain text, then the end card |
| `glassbox-dvp-sim --scenario S6` | The raw trace, one line per record |
| `glassbox-dvp-sim --list` | Scenario ids and names |
| `glassbox-dvp-sim --help` | The command reference |

Options (add them to any command above):

| Option | Meaning |
|---|---|
| `--seed N` | Random seed for network jitter and drops (default 1). Same seed, same run |
| `--speed S` | `slow` (50%), `normal` (100%), `fast` (200%), or 25 to 800 |
| `--paused` | Start paused; press `n` to step |
| `--no-color` | Plain text |
| `--set KEY=VALUE` | A setting from section 5 (repeatable; not with `--tour`) |

Exit codes: `0` the run finished, `2` bad usage, `3` the run did not finish
(stopped at the event limit).

### Make shortcuts

The binary is at `build/make/debug/glassbox-dvp-sim`.

| Shortcut | Same as |
|---|---|
| `make` | build, then the menu (`SPEED=`, `SEED=`, `SET=` set its starting options) |
| `make tour` | `--tour` |
| `make watch S=S6 SPEED=fast SEED=7 SET="vote_timeout=30ms drop.COORD-BANK_A=10%"` | `--scenario S6 --watch …` |
| `make story S=S6` | `--scenario S6 --story` |
| `make run S=S6` | `--scenario S6` |
| `make list` | `--list` |
| `make manual` | `--help` |
| `make build` | build only |
| `make test` / `make test-all` | run the tests (Debug / Debug + Release + UBSan) |
| `make help` | list the shortcuts |

### Reading the raw trace

```
6 t=1100 MSG_SEND COORD trade=1 msg=PREPARE#2 COORD->BANK_A detail=BUYER @Effects::release
```

This is trace record 6, at simulated time 1,100 µs: the coordinator sends
PREPARE (message #2) for trade 1 to BANK_A, for its buyer leg. The function that
did it was `Effects::release`.

## 7. Troubleshooting

- **The screen looks broken or wraps.** Make the terminal at least 80 columns
  wide (100+ is best).
- **The colors are hard to read.** Add `--no-color`.
- **"not a terminal: printing the story instead".** Watching needs an
  interactive terminal. When the output is piped or redirected, the player
  prints the text story instead.
- **"the tour needs a terminal".** Same reason; run it in a terminal window.
- **"Could not start S1: restart_after=2s: …".** That setting needs a scenario
  with a crash (6 to 10). Type `clear` to remove it.
- **The build fails with missing C++ headers.** The Makefile prefers Homebrew
  clang (`/opt/homebrew/opt/llvm/bin/clang++`). If the compiler it uses cannot
  find `<vector>`, it adds the macOS SDK headers itself. To use another
  compiler, run `make CXX_BIN=/path/to/clang++`, then `make clean`.
