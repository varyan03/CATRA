# CATRA — Project Context for Agents and Contributors

> **Purpose of this file:** a complete, self-contained briefing. Hand it to a coding agent (or a new human contributor) as the first message, and they should be able to work on CATRA without re-discovering anything. It records *what exists, how it works, why it was built that way, what has been verified, and what is still open*.
>
> Last updated: 2026-11-08 · Repo: https://github.com/varyan03/CATRA · Branch: `main` · License: MIT

---

## 0. Read this first — rules for anyone working on this repo

These are standing decisions by the project owner. Do not re-litigate them.

1. **Do not redesign the core mechanics.** Keep:
   - thread-level `nice` via `setpriority(PRIO_PROCESS, tid, ...)` (do **not** switch to cgroups),
   - `/proc` parsing with the `rfind(')')` approach,
   - the two-way hysteresis, PID-reuse (`starttime`) checks, and `/run/catra/state.dat` crash persistence.
   Additive changes (new features, new signals, better models) are welcome; replacing these is not.
2. **Linux only.** The daemon needs `/proc`, `/dev/pts`, evdev (`<linux/input.h>`) and root. It cannot run on macOS/Windows. The owner edits on a Mac and builds/tests on **AWS EC2 Amazon Linux 2023** via the **browser terminal**.
3. **Browser terminal breaks multi-line pastes.** When giving the owner shell commands, give **single-line commands** (no `\` continuations). Prefer scripts (`build.sh`, `tests/*.sh`) over long commands.
4. **No AI attribution in git.** Never add `Co-Authored-By: Claude` (or any AI co-author trailer / "Generated with" line) to commits or PRs. History was rewritten once to remove these.
5. **No emojis in `README.md`.**
6. **Don't edit generated model code** (`daemon/main.c`, `daemon/header.h`, `ml-pipeline/catra_treelite_c/*`). Regenerate it from the pipeline instead. All C-linkage handling lives in `daemon/ml_wrapper.cpp`.
7. Ask before pushing or force-pushing to `main`.

---

## 1. What CATRA is

**CATRA (Context-Aware Tiered Resource Allocation)** is a C++20 Linux daemon that automatically gives the user's active workload more CPU when the user is focused.

Every 2 seconds it:
1. counts keystrokes from a Linux input device (or a test FIFO),
2. computes typing speed (WPM) and terminal-switch rate,
3. runs a compiled XGBoost model to classify **Focus** vs **Relax**,
4. finds CPU-heavy **threads** in the user's active terminal session and promotes them to `nice -5`,
5. restores each thread's **exact original** nice when focus/CPU drops, on shutdown, or after a crash.

**Status: working proof of concept.** All mechanics verified end-to-end on EC2; measured 1.5× CPU-share gain (see §6).

**Target users:** Linux desktop developers with a physical keyboard. EC2 is only the test bench (it has no keyboard; a FIFO simulates one).

---

## 2. Repository map

```
catra-project/                      (git root; GitHub: varyan03/CATRA)
├── PROJECT_CONTEXT.md              this file
├── FUTURE_PLAN.md                  macOS / Windows porting plan (development + testing)
├── README.md                       public-facing docs (no emojis)
├── LICENSE                         MIT, "Copyright (c) 2026 varyan03 and CATRA contributors"
├── build.sh                        one-shot build → daemon/catra
├── catra.service                   systemd unit
├── .gitignore                      ignores *.csv, build outputs, binaries, Makefile(!)
├── daemon/
│   ├── catra_daemon.cpp            main loop (entry point)
│   ├── telemetry_sensor.{h,cpp}    async evdev/FIFO keystroke counter
│   ├── anchor_engine.{h,cpp}       scheduler core (targeting, hysteresis, crash state)
│   ├── ml_wrapper.{h,cpp}          C++ facade over the Treelite C model
│   ├── main.c, header.h            GENERATED Treelite predictor (copy of ml-pipeline export)
│   └── README.md                   (empty; root README is canonical)
├── ml-pipeline/
│   ├── generate_dataset.py         synthetic telemetry generator
│   ├── check_dataset.py            dataset QA gates + scatter plot
│   ├── train_and_export.py         XGBoost training → Treelite C export
│   ├── requirements.txt            numpy, pandas, scikit-learn, xgboost, treelite(+tl2cgen), matplotlib, seaborn
│   ├── dataset_scatter.png         QA plot (tracked)
│   ├── catra_treelite_c/           export output: main.c, header.h, recipe.json (tracked)
│   └── *.csv                       datasets (gitignored — regenerate locally)
└── tests/
    ├── catra_unit_tests.cpp        model assertions (raw predictor)
    ├── catra_test_suite.cpp        model prediction table
    ├── realtime_test.cpp           interactive: type a sentence → WPM → prediction
    ├── test_sensor.cpp             KeyboardSensor harness
    ├── test_anchor.cpp             AnchorEngine manual runner
    ├── test_anchor_automated.cpp   AnchorEngine automated promote/demote test
    ├── mock_keyboard.py            writes input_event records into /tmp/fake_keyboard
    ├── integration_test.sh         end-to-end daemon test (15 checks)
    └── benchmark.sh                CPU-share benchmark
```

Removed on purpose (do not resurrect): `daemon/scheduler.*` (old whole-process −10 boost), `daemon/process_tracker.*` (old `/proc` reader), `daemon/posix_queue_test.cpp`, `tests/test_os_scheduler.cpp`, `tests/test_tracker.cpp`. All superseded by `AnchorEngine`.

`daemon/catra_daemon` (if present locally) is a stale **macOS** binary; ignore it. The real build output is `daemon/catra`.

---

## 3. ML model and dataset

### 3.1 Features (order is load-bearing)
| Index | Name | Daemon source |
|---|---|---|
| `[0]` | `typing_speed_wpm` | 20 s rolling window of keystrokes; WPM = (keys/5) per minute |
| `[1]` | `window_switches_per_min` | 60 s rolling window counting active-TTY changes (proxy for GUI window switches) |

Label `target_state`: `1` = Focus, `0` = Relax.

### 3.2 Dataset — `generate_dataset.py`
- 5,000 rows, timestamps every 5 s from 2026-11-02 09:00:00.
- States from a Markov chain: P(stay Focus)=0.990, P(stay Relax)=0.985 → stationary ≈ 60% Focus / 40% Relax.
- Feature distributions (Gaussian):
  | State | WPM | Switches/min |
  |---|---|---|
  | Focus | N(72, 12) | N(1.8, 1.0) |
  | Relax | N(18, 8.5) | N(15, 3.8) |
- Jitter: WPM ±N(0,1.2), switches ±N(0,0.4). Clipped: WPM [0,160], switches [0,40].
- 5% label noise (random flips).
- CLI: `--rows 5000 --noise 0.05 --seed 42 --out catra_training_dataset.csv`.

### 3.3 QA — `check_dataset.py`
Gates: no nulls; Focus share in [50%, 70%]; WPM in [0,200], switches in [0,60]; writes `dataset_scatter.png`. Prints overall PASSED/FAILED.

### 3.4 Training/export — `train_and_export.py`
- 80/20 stratified split (`random_state=42`).
- `XGBClassifier(n_estimators=100, max_depth=3, learning_rate=0.1, eval_metric="logloss")`.
- Prints accuracy + classification report.
- Exports with `treelite.frontend.from_xgboost` + `tl2cgen.generate_c_code` → `ml-pipeline/catra_treelite_c/`.

### 3.5 Generated predictor API (`header.h`)
```c
union Entry { int missing; float fvalue; int qvalue; };
void predict(union Entry* data, int pred_margin, float* result);   // result[0] ACCUMULATES (+=)
void postprocess(float* result);                                    // sigmoid
int32_t get_num_feature(void);                                      // == 2
```
- `predict` **adds** tree outputs into `result[0]` → caller must zero it.
- `pred_margin=0` applies sigmoid → probability of Focus.
- `missing == -1` marks a missing feature; writing any finite `fvalue` overwrites it safely.
- `header.h` has **no** `extern "C"` guards; C++ callers must wrap the include.

### 3.6 Known model weakness (important)
The synthetic data never contains a fully idle user (Relax always has ~15 switches/min). Raw model outputs:
| WPM | Switches | P(focus) | Raw class |
|---|---|---|---|
| 0 | 0 | 0.517 | Focus ← wrong |
| 10 | 2 | 0.744 | Focus ← questionable |
| 35 | 0 | 0.700 | Focus |
| 90 | 1 | 0.908 | Focus |
| 25 | 15 | 0.047 | Relax |

Mitigation in the daemon: `kIdleWpmFloor = 5.0` in `ml_wrapper.cpp` forces Relax below 5 WPM. **This is a stopgap.** Proper fix: add idle / low-switch Relax samples to the generator and retrain (§9).

---

## 4. Daemon architecture

### 4.1 Data flow (one tick = 2 s)
```
/dev/input/eventX or /tmp/fake_keyboard
   │ struct input_event (24 bytes on 64-bit)
   ▼
KeyboardSensor (listener thread, atomic counter)
   │ get_and_reset_count()
   ▼
catra_daemon.cpp main loop
   ├─ keystroke_window (10 ticks = 20 s) → wpm
   ├─ tty_switch_window (30 ticks = 60 s) → switches/min
   ├─ predict_focus_probability(wpm, switches) → is_focused (> 0.5)
   ├─ engine.sweep(is_focused)
   └─ engine.get_active_tty() → record switch for next tick
```

### 4.2 `telemetry_sensor.{h,cpp}` — `KeyboardSensor`
- `start(path)`: `open(O_RDONLY|O_NONBLOCK)`, spawns `worker_loop(fd)` thread. Serialized by `lifecycle_mutex_`. Returns false if already running or open fails.
- `worker_loop`: `poll(100 ms)`; drains all pending `input_event`s; counts `ev.type == EV_KEY && ev.value == 1` (press only; release=0, autorepeat=2 ignored). Stops on EOF, `POLLHUP/POLLERR/POLLNVAL`, read error, or short read (desync).
- `get_and_reset_count()`: atomic `exchange(0)`, lock-free.
- `running()`: whether the stream is alive (main loop reconnects if false).
- On non-Linux it defines a stand-in `input_event` so it compiles on macOS (for syntax checks only).
- FIFO behaviour: opening a FIFO read-only non-blocking with no writer succeeds; when the writer closes, `POLLHUP` stops the sensor and the daemon reopens it next tick.

### 4.3 `catra_daemon.cpp` — main loop
- Requires `geteuid()==0`; usage `catra <device_or_fifo_path>`.
- `sigaction` for SIGINT/SIGTERM (no `SA_RESTART`) sets `shutdown_requested` (defined in `anchor_engine.cpp`, `volatile sig_atomic_t`).
- Constants: `INTERVAL=2`, `WPM_WINDOW_TICKS=10`, `SWITCH_WINDOW_TICKS=30`.
- `interruptible_sleep()` sleeps in 100 ms slices so shutdown is prompt.
- Sensor reconnect: if `!sensor.running()` → `stop()`, sleep 1 s, `start()` again.
- WPM uses only the **filled** part of the window (no dilution during the first 20 s).
- TTY switch counted only for genuine A→B changes; empty TTY never counts and never overwrites `previous_tty` (so A→""→A is not a switch). The switch detected after sweep N feeds prediction N+1 (≤2 s lag, intentional).
- Log line (flushed with `std::endl`, journald-friendly):
  `[CATRA] WPM: 54.6 | Switches/min: 0 | P(focus): 0.788 | State: FOCUS`
- Shutdown: `sensor.stop()`, `engine.restore_all()` (idempotent; destructor calls it again).

### 4.4 `ml_wrapper.{h,cpp}`
```cpp
float predict_focus_probability(double wpm, double switches_per_min) noexcept;
bool  predict_focus(double wpm, int tty_switches_per_min) noexcept;   // prob > 0.5
```
- Pre-includes `<stdio.h> <stdlib.h> <string.h> <float.h> <math.h> <stdint.h>` **before** `extern "C" { #include "header.h" }` to avoid libstdc++ "template with C linkage" errors.
- `static_assert(sizeof(union Entry) == sizeof(float))`.
- Sanitizes NaN/inf/negative → 0. Zero-inits `result[1] = {0}`. Non-finite output → 0.
- Idle guard: `wpm < kIdleWpmFloor (5.0)` → returns 0.0 without calling the model.

### 4.5 `anchor_engine.{h,cpp}` — `AnchorEngine` (core; single-threaded use only)
**Public API:** `sweep(bool ml_focus_signal)`, `restore_all()`, `check_autogroup()`, `get_active_tty()` (returns last resolved `/dev/pts/N`, empty until first success).

**Construction:**
- `determine_state_paths()`: root → `/run/catra`; else `$XDG_RUNTIME_DIR/catra` or `/tmp/catra_<uid>`. Dir forced to 0700. File: `state.dat`.
- `check_autogroup()`: warns if `/proc/sys/kernel/sched_autogroup_enabled == 1`.
- `recover_state()`: if `state.dat` exists — reject unless mode is exactly 0600 and (when root) owned by root; for each `tid:starttime:original_nice` line, restore only if `/proc/<tid>/stat` starttime matches and current nice is −5; then unlink.

**`sweep()` Phase A — measurement:**
1. `find_active_tty`: newest `st_atime` among `/dev/pts/*` (skips `ptmx`). Records owner uid, major/minor, `newest_pts_atime_`, `active_pts_path_`.
2. `collect_process_tree`: scans `/proc/[0-9]*`; keeps processes owned by target uid **or root**; **excludes** comms `bash, zsh, sh, fish, dash, tmux, screen, sshd, login` and session leaders (`pid == session`). Roots = processes whose decoded `tty_nr` matches the pts major/minor; BFS over a PPID→children map adds all descendants.
3. Per thread (`/proc/<pid>/task/<tid>/stat`):
   - new TID → store starttime/total time, no CPU% yet; baseline nice = current nice, **except** inherited-boost case (a sibling in same tgid is boosted and this thread is at −5 → baseline = sibling's `original_nice`);
   - starttime changed → PID reuse → reset state;
   - else `cpu_pct = Δ(utime+stime) / CLK_TCK / elapsed_secs × 100`.
4. `parse_stat_line`: comm = between first `(` and **last** `)`; tokens after `") "` indexed: `[0]` state, `[1]` ppid, `[2]` pgrp, `[3]` session, `[4]` tty_nr, `[5]` tpgid, `[11]` utime, `[12]` stime, `[19]` starttime. Requires ≥20 tokens.

**`sweep()` Phase B — decision:**
- `atime_warm = (now − newest_pts_atime_) < 30 s`
- `is_session_warm = atime_warm || ml_focus_signal`  ← ML can only *add* warmth; it never vetoes terminal activity.
- Per thread:
  | Branch | Condition | Counter | Action |
  |---|---|---|---|
  | Promote | active && cpu > 15% && warm | `promotion_ticks++`, demotion reset | at ≥2 and not boosted: re-verify starttime, save current nice as original (if not −5), `setpriority(tid, -5)` |
  | Demote | !active \|\| cpu < 5% \|\| !warm | `demotion_ticks++`, promotion reset | at ≥3 and boosted: re-verify; if same thread and still −5 → restore `original_nice` |
  | Dead band | 5–15% CPU while warm | unchanged | none |
- GC: unboosted entries removed if `/proc/.../task/<tid>` is gone or idle > 20 sweeps.
- `persist_state()` when anything changed: write `state.dat.tmp.<pid>` (0600) then `rename()` atomically; if nothing is boosted, unlink both.
- Logs per sweep: `[AnchorEngine] State: TTY=/dev/pts/1 | atime_age=46s (warm=no) | ml_signal=FOCUS => SessionWarm=TRUE | InScope=4 | Threads=6`, plus `Promoted TID ...` / `Demoted TID ...` lines.

**`restore_all()`**: restore every boosted thread whose starttime matches and nice is −5; clear map; unlink state file.

### 4.6 Build — `build.sh`
```
gcc -O3 -std=c11 -c main.c -o model.o
g++ -std=c++20 -O3 -pthread -Wall -Wextra catra_daemon.cpp telemetry_sensor.cpp anchor_engine.cpp ml_wrapper.cpp model.o -lm -o catra
```
(run from `daemon/`; the script handles `cd`). Output: `daemon/catra`. Note `.gitignore` ignores `Makefile`, which is why a shell script is used.

### 4.7 Deployment — `catra.service`
Key settings and why:
- `ExecStartPre=/bin/sh -c '[ -p /tmp/fake_keyboard ] || mkfifo -m 0622 /tmp/fake_keyboard'` (EC2/test mode); `ExecStart=/usr/local/bin/catra /tmp/fake_keyboard`. For real hardware: drop ExecStartPre and point at `/dev/input/by-id/*-event-kbd`.
- `RuntimeDirectory=catra`, `RuntimeDirectoryMode=0700`, **`RuntimeDirectoryPreserve=restart`** — without this systemd deletes `/run/catra` on crash-restart and destroys `state.dat` exactly when recovery needs it.
- `Restart=on-failure`, `RestartSec=5`, `StartLimitIntervalSec=60`, `StartLimitBurst=10`, `KillSignal=SIGTERM`, `TimeoutStopSec=10`.
- `After=local-fs.target systemd-udevd.service` (no network dependency).
- Hardening: `PrivateTmp=no` (FIFO must be shared), `NoNewPrivileges`, `ProtectSystem=full`, `ProtectHome=read-only`, `ProtectKernelModules`, `ProtectControlGroups`, `RestrictRealtime`, `LockPersonality`.

Install:
```
sudo install -m 0755 daemon/catra /usr/local/bin/catra
sudo install -m 0644 catra.service /etc/systemd/system/catra.service
sudo systemctl daemon-reload && sudo systemctl enable --now catra
journalctl -u catra -f
```

---

## 5. Testing

Run everything from the repo root on Linux. Single-line commands.

### 5.1 Setup
```
bash build.sh
mkdir -p build
gcc -O2 -c daemon/main.c -o build/model.o
```
(`build/` is gitignored.)

### 5.2 Model tests (no root)
```
g++ -std=c++20 -O2 tests/catra_unit_tests.cpp build/model.o -lm -o build/catra_unit_tests && ./build/catra_unit_tests
g++ -std=c++20 -O2 tests/catra_test_suite.cpp build/model.o -lm -o build/catra_test_suite && ./build/catra_test_suite
g++ -std=c++20 -O2 tests/realtime_test.cpp -lm -o build/realtime_test && ./build/realtime_test
```
- `catra_unit_tests` and `catra_test_suite` include `header.h` in `extern "C"` and link `model.o`.
- `realtime_test` `#include`s `../daemon/main.c` directly (no `model.o`).
- ⚠️ `catra_unit_tests` asserts `(0 WPM, 0 switches) → Focus` and `(5, 2) → Focus`. That encodes the old raw-model behaviour; the daemon's idle guard overrides it. Update these assertions when the model is retrained.

### 5.3 Component tests
```
g++ -std=c++20 -O2 -pthread tests/test_sensor.cpp daemon/telemetry_sensor.cpp -o build/test_sensor && sudo ./build/test_sensor /tmp/fake_keyboard
python3 tests/mock_keyboard.py            # in a second terminal
g++ -std=c++20 -O2 -pthread -Idaemon tests/test_anchor_automated.cpp daemon/anchor_engine.cpp -o build/test_anchor_automated && sudo ./build/test_anchor_automated
g++ -std=c++20 -O2 -pthread tests/test_anchor.cpp daemon/anchor_engine.cpp -o build/test_anchor && sudo ./build/test_anchor focus
```
`mock_keyboard.py` options: default 10 keys @0.5 s; `--wpm W --duration S` for drift-free steady typing; `--fifo PATH`, `--count`, `--interval`, `--quiet`. Packs `struct.pack("@llHHi", sec, usec, EV_KEY=1, KEY_SPACE=57, 1)`.

### 5.4 Integration test — `tests/integration_test.sh` (~4 min, root)
```
sudo bash tests/integration_test.sh
```
**Do not type in any terminal while it runs** (the engine targets the newest-atime pts). Uses a `yes` burner and polls its nice value. Phases:
1. TTY path: just-used terminal → burner −5.
2. Idle guard: terminal cold, 0 WPM → RELAX → burner 0.
3. **ML path**: terminal cold + mock 80 WPM → burner −5; asserts the sweep line at promotion contains `warm=no` and `ml_signal=FOCUS`.
4. ML release: typing stops → burner 0.
5. SIGKILL: boosted → `kill -9` → state.dat present, burner stuck at −5 → restart → "Crash Recovery" logged, burner 0.
6. SIGTERM: boosted → graceful stop → burner 0, state.dat removed, shutdown logged.
Preflight refuses if not root, binary missing, `catra.service` active, or another `catra` running. Log at `/tmp/catra_it_log.*`.

### 5.5 Benchmark — `tests/benchmark.sh` (~2.5 min, root)
```
sudo bash tests/benchmark.sh
```
FG `yes` (in the terminal, in scope) vs NOISE `yes` (`setsid`, no tty, out of scope), both `taskset -c 0`. Daemon + mock run on core 1 when `nproc > 1`. Measures FG share of `utime+stime` over 15 s, CATRA off vs on (mock 80 WPM), for `sched_autogroup_enabled` = 1 and 0; restores the original sysctl on exit.

---

## 6. Verified results (EC2, Amazon Linux 2023, 2 vCPU, 2026-11-03)

- **Integration test: 15/15 PASS.** Key evidence line at ML-only promotion:
  `atime_age=46s (warm=no) | ml_signal=FOCUS => SessionWarm=TRUE`
  P(focus) rose 0.700 → 0.763 → 0.788 as WPM rose 38 → 55. Crash state file sample: `38960:624529:0`.
- **Benchmark:**
  | autogroup | FG share off | FG share CATRA | speedup |
  |---|---|---|---|
  | 1 | 50.0% | 75.3% | 1.51× |
  | 0 | 50.1% | 75.3% | 1.50× |
  75.3% = CFS weight ratio 3121/(3121+1024) for nice −5 vs 0.
  Autogroup had **no** effect on EC2 (original value was 0). Likely reason: systemd places sessions in cgroups, and autogroup only applies to tasks in the root task group. Not yet confirmed on a desktop.
- **Model tests (macOS, raw predictor):** unit tests pass; prediction table as in §3.6.

---

## 7. Design decisions and rationale (history)

| Decision | Why |
|---|---|
| Thread-level `nice`, not cgroups | Owner requirement; zero-config, per-thread precision |
| Treelite C export instead of Python inference | No runtime deps in a root daemon; microsecond inference |
| `ml_wrapper` instead of editing `header.h` | Generated files stay regenerable; linkage concerns isolated |
| Zero `result[0]` before `predict` | Generated code accumulates with `+=` |
| Pre-include std headers before `extern "C"` | libstdc++ `<math.h>` → `<cmath>` templates fail under C linkage |
| TTY switches as window-switch proxy | Headless/terminal context has no GUI focus events |
| Switch recorded after sweep, used next tick | Active TTY is only resolved inside `sweep()` |
| Idle guard `kIdleWpmFloor = 5` | Model says Focus at (0,0); with OR-warmth an idle user would stay boosted forever |
| `RuntimeDirectoryPreserve=restart` | Default deletes `state.dat` on crash-restart |
| FIFO mode 0622 | mock_keyboard.py can write without sudo; only root reads |
| `build.sh` instead of Makefile | `.gitignore` excludes `Makefile`; browser terminal mangles multi-line pastes |
| Removed scheduler/process_tracker prototypes | Superseded by AnchorEngine; unused by daemon/build |

Commit history of this phase (on `main`): `a7817ad` model + TTY switches + service + tests · `b39b05d` build.sh · `5a9835a` benchmark · `1ad72a6` README + LICENSE · `2e25795` README emojis removed · `47d5fae` prototypes removed · `d928a62` daemon/README.md emptied (owner).

---

## 8. Known limitations and open issues

1. **Synthetic training data**; model mis-handles idle/low-activity input (§3.6). Idle guard is a stopgap.
2. **`catra_unit_tests.cpp` encodes idle → Focus**; inconsistent with daemon behaviour; update after retrain.
3. **Terminal-scoped only**: GUI apps (IDE build buttons, browsers) are never targeted.
4. **cgroup boundaries**: on desktops each app may be in its own `app-*.scope` cgroup with the cpu controller; `nice` does not compete across cgroups, so gains vs GUI load are unproven.
5. **No physical keyboard tested**; evdev path implemented but only exercised via FIFO. Multiple keyboard devices are not aggregated (one path per daemon).
6. **SSH/browser-terminal typing is invisible to the sensor** (goes through the pty, not `/dev/input`); on servers, WPM is always 0 and only the atime path boosts.
7. `ml_focus_signal` is OR-ed with atime warmth — the model can add boosts but cannot suppress them.
8. Thresholds (15%/5%, 2/3 ticks, 30 s atime, −5) are hard-coded; no config file.
9. `daemon/README.md` is empty.
10. No CI.

---

## 9. How to extend — recipes

### 9.1 Retrain the model (highest-value next step)
1. In `generate_dataset.py`, add Relax samples for idle (WPM≈0–8, switches≈0–3) and light activity; keep Focus as is. Consider bumping noise realism.
2. Run (Mac or Linux):
   ```
   cd ml-pipeline && python3 -m venv .venv && source .venv/bin/activate && pip install -r requirements.txt
   python generate_dataset.py && python check_dataset.py && python train_and_export.py
   cp catra_treelite_c/main.c catra_treelite_c/header.h ../daemon/
   ```
3. Verify raw outputs: (0,0) and (10,2) should be Relax; (72,1.8) Focus.
4. Remove or lower `kIdleWpmFloor` in `ml_wrapper.cpp`; update `catra_unit_tests.cpp` assertions.
5. `bash build.sh`, then rerun `integration_test.sh` (phase 2 and 3 are the relevant checks).

### 9.2 Add a new model feature
1. Add the column in `generate_dataset.py`, `check_dataset.py` bounds, and `feature_cols` in `train_and_export.py` (order matters).
2. Re-export and copy generated files.
3. Extend `ml_wrapper` signatures and fill `features[2]`, etc.; bump `kNumFeatures`.
4. Compute the feature in `catra_daemon.cpp` (add a rolling window if needed) and log it.

### 9.3 Change thresholds / tiers
All in `AnchorEngine::sweep()` (Phase B) and `catra_daemon.cpp` constants. If changing −5, also update every `cur_nice == -5` / `!= -5` check (promotion, demotion, `recover_state`, `restore_all`) and the tests (`integration_test.sh`, `benchmark.sh`, `test_anchor_automated.cpp`) that assert −5. Keep the starttime re-verification before every `setpriority`.

### 9.4 Real desktop deployment
Find the keyboard: `ls -l /dev/input/by-id/*-event-kbd`; set `ExecStart=/usr/local/bin/catra <that path>`; remove `ExecStartPre`. Consider `sudo sysctl -w kernel.sched_autogroup_enabled=0` and verify gains with `benchmark.sh` adapted to GUI-scope noise.

### 9.5 Verify any change
Mac (syntax only): `clang++ -std=c++20 -Wall -Wextra -fsyntax-only daemon/catra_daemon.cpp daemon/ml_wrapper.cpp`.
Linux (real): `bash build.sh` → model tests → `test_anchor_automated` → `integration_test.sh` → `benchmark.sh`.

---

## 10. Roadmap (owner-agreed priorities)
1. Retrain the model with idle/low-activity samples; drop the idle guard.
2. Validate on a Linux desktop with a physical keyboard and GUI load (cgroup question).
3. Optional: real window-focus signal (X11/Wayland), multi-tier boosts by confidence, config file, CI.
4. macOS and Windows ports — full development and testing plan in [`FUTURE_PLAN.md`](FUTURE_PLAN.md).
