# CATRA — Context-Aware Tiered Resource Allocation

> **How might we build a CPU scheduler that notices when a developer is actively working and gives their workload more CPU automatically, with no manual `nice` or `renice` and no configuration?**

## Overview

CATRA is a Linux background daemon, written in C++20, that changes CPU priority based on context. It watches the user's keyboard and terminal activity and runs a compiled XGBoost model to decide whether the user is in a **Focus** or **Relax** state. When the user is focused, CATRA finds the CPU-heavy threads in their active terminal session, such as a compile, a test run or a script, and promotes them to a higher scheduling tier (`nice -5`). When the user stops, it puts every thread back to its exact original priority.

It works directly with the kernel through `/proc`, `/dev/pts` and `/dev/input`, has no runtime dependencies, and runs as a `systemd` service.

### The Problem

Developer machines are rarely idle. The workload the user is waiting on competes with everything else:

- **Background noise steals cycles**: indexers, sync clients, browsers and other background work compete equally with the user's build
- **The OS has no context**: the default scheduler can't tell a compile the user is watching from background work nobody is waiting on
- **Manual tuning doesn't happen**: almost nobody runs `renice` on their compile, and nobody remembers to undo it afterwards
- **Static priorities go stale**: a priority that's right while the user is focused is wrong once they walk away

### The Solution

An autonomous daemon that combines:

- **Activity telemetry**: raw keystroke counts from evdev plus terminal-switch frequency
- **On-device ML inference**: an XGBoost classifier compiled to plain C with Treelite, so there is no Python at runtime
- **Session-scoped targeting**: only threads in the user's active terminal session are touched
- **Hysteresis**: requires several consecutive readings before promoting or demoting, so the boost doesn't flicker on and off
- **Crash-safe state**: every boost is recorded on disk and undone after a crash, even a `SIGKILL`

## Results

All results below are from an **AWS EC2 instance running Amazon Linux 2023 (2 vCPU)**.

### Benchmark: CPU share under contention

Two CPU-bound processes were pinned to the same core. One ran in the user's terminal (managed by CATRA) and one was background noise (not managed). Script: [`tests/benchmark.sh`](tests/benchmark.sh).

| `sched_autogroup` | User workload, CATRA off | User workload, CATRA on | Speedup |
|:---:|:---:|:---:|:---:|
| 1 | 50.0% | **75.3%** | **1.51×** |
| 0 | 50.1% | **75.3%** | **1.50×** |

The measured 75.3% matches the scheduler's (CFS) weight ratio for nice −5 against 0 exactly: `3121 / (3121 + 1024) = 75.3%`.

### Integration test: 15/15 checks passed

Script: [`tests/integration_test.sh`](tests/integration_test.sh).

| Phase | Verified behaviour |
|---|---|
| Terminal-activity path | A recently used terminal promotes its CPU-heavy thread to −5 within about 5s |
| Idle guard | Terminal cold and 0 WPM → model says RELAX → thread demoted |
| **ML path** | Terminal idle for 46s, mock typing at 80 WPM → promoted with `warm=no, ml_signal=FOCUS`, so the model alone made the decision |
| ML release | Typing stops → WPM decays → demoted |
| `SIGKILL` recovery | Daemon killed while boosting → `state.dat` survives → restarted daemon restores the original priority |
| `SIGTERM` shutdown | Graceful stop restores every priority and removes the state file |

## Core Features

### Headless Keystroke Telemetry

- Reads `struct input_event` records directly from `/dev/input/event*`, with no X11/Wayland dependency
- Counts key presses only; key releases and auto-repeat are ignored
- A non-blocking `poll` loop runs on its own thread with a lock-free atomic counter
- Also accepts a named FIFO, so testing works on headless servers (EC2, WSL)
- Reconnects automatically when the device or stream goes away

### Compiled ML Inference

- XGBoost model: 100 trees, max depth 3, trained on `typing_speed_wpm` and `window_switches_per_min`
- Exported to dependency-free C with **Treelite / TL2cgen** and linked straight into the daemon
- Inference is a few hundred branch comparisons, taking microseconds per tick
- The C++ wrapper (`ml_wrapper.cpp`) keeps the C-linkage details out of the rest of the code and protects against out-of-range inputs

### Session-Scoped Targeting (Anchor Engine)

- Identifies the user's active terminal as the `/dev/pts/*` with the newest access time
- Builds a parent/child process tree from `/proc/*/stat` and collects every process attached to that terminal, plus all their descendants (this catches background jobs like `make &`)
- Excludes shells, multiplexers and session leaders (`bash`, `zsh`, `tmux`, `sshd`, …)
- Measures CPU per **thread** from `/proc/[pid]/task/[tid]/stat`, not per process

### Two-Way Hysteresis

| Transition | Condition | Ticks required |
|---|---|---|
| **Promote → −5** | thread CPU > 15% **and** session warm | 2 consecutive |
| **Demote → original** | CPU < 5% **or** thread idle **or** session cold | 3 consecutive |

A session counts as **warm** if the terminal was used in the last 30s **or** the ML model reports Focus.

### Safety & Correctness

- **PID-reuse protection**: before every priority change, CATRA checks the thread's `starttime` to make sure it is still the same thread
- **Inherited-boost detection**: a new thread that inherited −5 from a boosted parent gets the parent's *original* priority recorded as its baseline, not −5
- **Parser that handles odd process names**: `/proc` stat lines are split at the last `)`, so process names containing spaces or parentheses can't break parsing
- **Crash recovery**: active boosts are written to `/run/catra/state.dat` (0600, written atomically via temp file + `rename`). On startup, any leftover boosts are restored
- **Signal-safe shutdown**: `SIGINT` and `SIGTERM` trigger `restore_all()`, which can safely run more than once
- **Exact restore**: threads go back to the *exact* priority they had before; CATRA never assumes the original was 0

## How It Works

Each tick (every 2 seconds) runs the same pipeline:

```
 /dev/input/eventX  (or FIFO)
          │  EV_KEY press events
          ▼
 ┌──────────────────┐   keystrokes per tick
 │ KeyboardSensor   │──────────────────────────┐
 └──────────────────┘                          ▼
                                   ┌──────────────────────────┐
                                   │ Rolling windows          │
                                   │  • 20 s → WPM            │
                                   │  • 60 s → TTY switches   │
                                   └────────────┬─────────────┘
                                                ▼
                                   ┌──────────────────────────┐
                                   │ XGBoost (Treelite C)     │
                                   │  P(focus) > 0.5 → FOCUS  │
                                   └────────────┬─────────────┘
                                                ▼  ml_focus_signal
 ┌──────────────────────────────────────────────────────────────┐
 │ AnchorEngine::sweep()                                        │
 │  A. find active /dev/pts → build process tree → thread CPU % │
 │  B. warm = (terminal used < 30 s) OR ml_focus_signal         │
 │     hysteresis → setpriority(tid, -5) / restore original     │
 │     persist /run/catra/state.dat                             │
 └──────────────────────────────────────────────────────────────┘
```

### Phases of a typical session

1. **Idle**: no typing and the terminal is cold. The model says RELAX and nothing is boosted.
2. **Focus begins**: the user types and starts `make -j`. WPM rises and P(focus) goes above 0.5. After 2 ticks, compiler threads using more than 15% CPU are promoted to −5.
3. **Sustained work**: boosted threads keep their tier while the session stays warm. New compiler threads are picked up as they appear.
4. **Wind-down**: typing stops, WPM decays over the 20s window, and the state flips to RELAX. After 3 ticks, every thread is restored to its original priority.

## Architecture

| Component | File(s) | Responsibility |
|---|---|---|
| **Master daemon** | [`daemon/catra_daemon.cpp`](daemon/catra_daemon.cpp) | 2s control loop, rolling windows, signal handling, logging |
| **Telemetry sensor** | [`daemon/telemetry_sensor.{h,cpp}`](daemon/telemetry_sensor.h) | Async evdev/FIFO keystroke counter |
| **Anchor engine** | [`daemon/anchor_engine.{h,cpp}`](daemon/anchor_engine.h) | Terminal discovery, process tree, thread CPU, hysteresis, crash state |
| **ML wrapper** | [`daemon/ml_wrapper.{h,cpp}`](daemon/ml_wrapper.h) | C++ interface over the Treelite model, input checks, idle guard |
| **Compiled model** | [`daemon/main.c`](daemon/main.c), [`daemon/header.h`](daemon/header.h) | Treelite-generated XGBoost predictor (generated code, do not edit) |
| **ML pipeline** | [`ml-pipeline/`](ml-pipeline/) | Dataset synthesis, quality checks, training, C export |
| **Deployment** | [`catra.service`](catra.service) | systemd unit with crash-safe runtime directory |
| **Tests** | [`tests/`](tests/) | Unit tests, integration test, benchmark, mock keyboard |

**Stack:** C++20 · C11 · POSIX/Linux syscalls · Python 3 (training only) · XGBoost · Treelite/TL2cgen · systemd

## Getting Started

### Prerequisites

- **Linux** (CATRA requires `/proc`, `/dev/pts` and evdev; it does not run on macOS or Windows)
- `gcc` and `g++` with C++20 support (GCC 10+)
- `python3` (for the mock keyboard and the ML pipeline)
- Root access (CATRA needs root to call `setpriority` on other users' threads)

```bash
# Amazon Linux / Fedora / RHEL
sudo dnf install -y gcc gcc-c++ python3 git
# Debian / Ubuntu
sudo apt install -y build-essential python3 git
```

### Build

```bash
git clone https://github.com/varyan03/CATRA.git
cd CATRA
bash build.sh            # → daemon/catra
```

### Run manually

```bash
# Real keyboard (find yours under /dev/input/by-id/*-event-kbd)
sudo ./daemon/catra /dev/input/by-id/<your-keyboard>-event-kbd

# Headless / cloud testing with a simulated keyboard
sudo ./daemon/catra /tmp/fake_keyboard                         # terminal 1
python3 tests/mock_keyboard.py --wpm 80 --duration 40          # terminal 2
```

Example log output:

```
[AnchorEngine] State: TTY=/dev/pts/1 | atime_age=46s (warm=no) | ml_signal=FOCUS => SessionWarm=TRUE | InScope=4 | Threads=6
[AnchorEngine] Promoted TID 38960 (yes) to Tier 1 (-5) | CPU: 99.5%
[CATRA] WPM: 54.6 | Switches/min: 0 | P(focus): 0.788 | State: FOCUS
```

Press `Ctrl+C` to stop; every priority is restored on exit.

### Test

Don't type in any terminal while these run: CATRA targets the most recently used terminal.

```bash
sudo bash tests/integration_test.sh   # ~4 min, 15 end-to-end checks
sudo bash tests/benchmark.sh          # ~2.5 min, CPU-share measurement
```

### Deploy as a service

```bash
sudo install -m 0755 daemon/catra /usr/local/bin/catra
sudo install -m 0644 catra.service /etc/systemd/system/catra.service
# For real hardware, edit ExecStart= to point to your keyboard device
sudo systemctl daemon-reload
sudo systemctl enable --now catra
journalctl -u catra -f
```

The unit uses `RuntimeDirectory=catra` with `RuntimeDirectoryPreserve=restart`, so `/run/catra/state.dat` survives automatic restarts after a crash and recovery always has the data it needs.

### Retrain the model

```bash
cd ml-pipeline
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
python generate_dataset.py                 # → catra_training_dataset.csv
python check_dataset.py                    # quality checks + scatter plot
python train_and_export.py                 # → catra_treelite_c/{main.c,header.h}
cp catra_treelite_c/main.c catra_treelite_c/header.h ../daemon/
cd .. && bash build.sh
```

## Project Structure

```
CATRA/
├── build.sh                    # one-shot build → daemon/catra
├── catra.service               # systemd deployment unit
├── daemon/
│   ├── catra_daemon.cpp        # master control loop
│   ├── telemetry_sensor.*      # evdev / FIFO keystroke sensor
│   ├── anchor_engine.*         # scheduler core
│   ├── ml_wrapper.*            # C++ ↔ Treelite bridge
│   └── main.c, header.h        # generated XGBoost predictor
├── ml-pipeline/
│   ├── generate_dataset.py     # synthetic telemetry generator
│   ├── check_dataset.py        # dataset QA + plots
│   ├── train_and_export.py     # XGBoost → Treelite C export
│   └── catra_treelite_c/       # export output
└── tests/
    ├── integration_test.sh     # end-to-end daemon test
    ├── benchmark.sh            # CPU-share benchmark
    ├── mock_keyboard.py        # simulated evdev keyboard
    └── test_*.cpp, catra_*.cpp # component tests
```

## Known Limitations

- **Synthetic training data**: the model is trained on generated telemetry. With no typing it is unreliable, so a 5-WPM idle guard in `ml_wrapper.cpp` forces RELAX below that speed until the model is retrained on real data.
- **Terminal-scoped only**: CATRA only boosts processes attached to a terminal (including terminals inside IDEs). GUI applications are not targeted.
- **cgroup boundaries**: `nice` only applies between processes in the same CPU cgroup. On desktops where systemd puts each app in its own cgroup, a boost may not beat GUI apps in other cgroups.
- **TTY-switch proxy**: "window switches" are approximated by switches between active terminals; real GUI focus changes are not measured.
- **Validated on EC2 only**: testing used a FIFO-simulated keyboard; the evdev path is implemented but has not been tested on physical hardware yet.

## Future Enhancements

- Retrain on real, consented telemetry, including idle and low-activity samples
- Desktop validation with a physical keyboard and GUI workloads
- Real window-focus signals via Wayland/X11, as an optional feature
- Optional cgroup-aware targeting for GUI app scopes
- Multiple boost tiers (−5 / −10) based on model confidence
- Configuration file for thresholds, interval and excluded process names

## Contributing

Contributions are welcome. Please open an issue to discuss significant changes first, and include output from `tests/integration_test.sh` with any pull request that touches the daemon.

## License

CATRA is open source under the [MIT License](LICENSE). You are free to use, modify and distribute it, including in commercial projects, as long as the copyright notice is kept.

## Contact

For questions or support, please [open an issue](https://github.com/varyan03/CATRA/issues).

---

**Built for developers who'd rather wait less on their builds**
