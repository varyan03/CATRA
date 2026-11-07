# CATRA — Future Plan: macOS and Windows Ports

> **Audience:** a coding agent or developer implementing CATRA on macOS and/or Windows.
> **Read first:** [`PROJECT_CONTEXT.md`](PROJECT_CONTEXT.md). It describes the working Linux implementation, the owner's rules, and the verified results. This plan builds on it and does not repeat it.
>
> Status of this document: **plan only — nothing here is implemented yet.** Items marked **[VERIFY]** are platform behaviours believed correct but not yet confirmed on real hardware for this project; confirm them in the spike tasks before building on them.
>
> Created: 2026-11-08

---

## 0. Ground rules for this work

1. **The Linux implementation is the reference and must not regress.** Do not change `daemon/anchor_engine.*`, `daemon/telemetry_sensor.*` or `daemon/catra_daemon.cpp` behaviour while porting. Every phase that touches shared code must end with a green Linux run of `tests/integration_test.sh` (15/15) and `tests/benchmark.sh` (~75% FG share).
2. **Same concept, native mechanisms.** Each port reproduces CATRA's *semantics* — measure activity → model → boost the user's active workload → restore exactly → survive crashes — using that OS's own APIs. Do not emulate Linux (`/proc`, evdev) on other systems.
3. **Privacy:** keystroke sensors only **count** key-down events. Never record key codes, characters or timestamps per key. This matters doubly on Windows/macOS, where keyboard hooks look like keyloggers to security tools and users.
4. **Owner rules still apply:** no AI co-author lines in commits, no emojis in README, single-line shell commands for the owner, ask before pushing.
5. **Open decisions (§9) need the owner's answer** before the phase that depends on them starts. Do not guess them silently.

---

## 1. Platform comparison (what changes per OS)

| Concern | Linux (done) | macOS (planned) | Windows (planned) |
|---|---|---|---|
| Keystroke source | evdev `/dev/input/eventX` (root) | `CGEventTap`, listen-only, `kCGEventKeyDown` (needs **Input Monitoring** permission) | `WH_KEYBOARD_LL` low-level hook or Raw Input (must run in the **user's desktop session**) |
| Test keystroke source | FIFO + `mock_keyboard.py` | same FIFO path (sensor already compiles on macOS) | named pipe mock, or `SendInput` (events carry `LLKHF_INJECTED`) |
| Active context | newest-atime `/dev/pts/N` | frontmost app (`NSWorkspace`) + its tty via `proc_bsdinfo.e_tdev`; `/dev/ttys*` atime **[VERIFY]** | foreground window → PID (`GetForegroundWindow` + `GetWindowThreadProcessId`) |
| Window-switch feature | TTY-change proxy | **real** app switches (`NSWorkspaceDidActivateApplicationNotification`) | **real** foreground changes (`SetWinEventHook(EVENT_SYSTEM_FOREGROUND)`) |
| Process enumeration | `/proc/*/stat` | `libproc`: `proc_listallpids`, `proc_pidinfo(PROC_PIDTBSDINFO / PROC_PIDTASKINFO)` | `CreateToolhelp32Snapshot` (processes + threads) |
| PID-reuse token | `starttime` (field 22) | `pbi_start_tvsec/usec` | process/thread creation time (`GetProcessTimes` / `GetThreadTimes`) |
| CPU time | `utime+stime` ticks | `pti_total_user+pti_total_system` (Mach time units on Apple Silicon → convert with `mach_timebase_info`) **[VERIFY]** | `GetThreadTimes` kernel+user, 100 ns units |
| Boost granularity | **per thread** | **per process** (no external per-thread priority API without task ports) | **per thread** (`SetThreadPriority`) or per process (`SetPriorityClass`) |
| Boost mechanism | `setpriority(tid, -5)` | `setpriority(PRIO_PROCESS, pid, -5)` (root) | `THREAD_PRIORITY_ABOVE_NORMAL` (+1) — see §9 |
| Scheduler model | CFS weights (proportional) | XNU timeshare + QoS; P/E cores on Apple Silicon | **strict priority** + dynamic boosts + balance-set anti-starvation |
| Expected effect | 50% → 75.3% measured | unknown — measure; likely smaller than Linux | likely **much larger** than Linux (strict priority) — watch for background starvation |
| Privilege | root | root for negative nice; user session for event tap → two-process design | admin to raise other users' threads; user session for hooks → two-process design |
| Crash state | `/run/catra/state.dat` | `/Library/Application Support/CATRA/state.dat` (root, 0600) | `%ProgramData%\CATRA\state.dat` (ACL: SYSTEM + Administrators) |
| Supervisor | systemd | launchd (`LaunchAgent` + `LaunchDaemon`) | Windows Service + per-user logon task |
| Compiler | gcc/g++ | Apple clang (C++20) | MSVC `cl /std:c++20` or `clang-cl` |

Key insight: **Linux runs as one root process because evdev is readable by root anywhere.** On macOS and Windows, keyboard events are only visible inside the user's interactive session, while raising priority needs elevation. Both ports therefore need either (a) one elevated process launched *inside* the user session, or (b) a split into a user-session **agent** and a privileged **helper**. See §9 decision D1.

---

## 2. Target architecture

### 2.1 Layers
```
                 ┌───────────────────────────────────────────┐
  portable core  │ RollingWindow · FocusModel (ml_wrapper)   │
  (new, shared)  │ HysteresisPolicy · StateCodec · DaemonLoop│
                 └───────────────┬───────────────────────────┘
                                 │ interfaces
       ┌─────────────────────────┼─────────────────────────┐
       ▼                         ▼                         ▼
  Linux (existing,          macOS (new)               Windows (new)
  untouched code in         platform/macos/           platform/windows/
  daemon/)
```

### 2.2 Interfaces (`platform/platform.h`, new)
```cpp
struct Unit {                 // what gets boosted: a thread (Linux/Windows) or process (macOS)
    uint64_t id;              // tid or pid
    uint64_t owner_pid;       // tgid / pid
    uint64_t identity_token;  // start time; guards against ID reuse
};

class IKeystrokeSource {      // mirrors KeyboardSensor's API exactly
public:
    virtual ~IKeystrokeSource() = default;
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual int  get_and_reset_count() noexcept = 0;   // key-DOWN only, no repeats
    virtual bool running() const noexcept = 0;
};

class IContextProvider {      // "what is the user working in right now?"
public:
    virtual ~IContextProvider() = default;
    virtual std::optional<std::string> active_context_id() = 0; // tty path or foreground pid as string
    virtual double seconds_since_context_activity() = 0;       // Linux: pts atime age
    virtual std::vector<uint64_t> root_pids() = 0;             // processes anchoring the active context
    virtual int  take_switch_count() = 0;                      // switches since last call (real or proxy)
};

class IProcessEnumerator {
public:
    virtual ~IProcessEnumerator() = default;
    struct Proc { uint64_t pid, ppid, start_token; std::string name; bool session_leader; };
    virtual std::vector<Proc> snapshot() = 0;
    virtual std::vector<Unit> units_of(uint64_t pid) = 0;            // threads or {pid}
    virtual std::optional<uint64_t> cpu_time_ns(const Unit&) = 0;    // nullopt if gone
};

class IPriorityActuator {
public:
    virtual ~IPriorityActuator() = default;
    virtual std::optional<int> current(const Unit&) = 0;   // platform-native priority value
    virtual bool boost(const Unit&) = 0;                   // to the platform's "Tier 1"
    virtual bool restore(const Unit&, int original) = 0;
    virtual bool is_boosted_value(int v) const = 0;        // Linux: v == -5
};

class IStateStore {
public:
    virtual ~IStateStore() = default;
    virtual void save(const std::vector<std::tuple<Unit,int>>& boosted) = 0;  // atomic replace
    virtual std::vector<std::tuple<Unit,int>> load_and_clear() = 0;         // with permission checks
};
```

### 2.3 Portable core (`core/`, new)
- `rolling_window.h` — fixed-capacity sum window (replaces the deque helpers; header-only).
- `hysteresis_policy.{h,cpp}` — **pure** state machine: per-unit `promotion_ticks`, `demotion_ticks`, dead band, thresholds. Inputs: `(cpu_pct, active, warm)`; output: `None | Promote | Demote`. Constants default to Linux values (15%/5%, 2/3 ticks). No syscalls → fully unit-testable on any OS.
- `portable_engine.{h,cpp}` — the macOS/Windows equivalent of `AnchorEngine::sweep()`, built only on the interfaces above + `HysteresisPolicy`. Includes inherited-boost handling, re-verification of `identity_token` before every actuation, GC, and persist-on-change.
- `daemon_loop.{h,cpp}` — the 2 s loop from `catra_daemon.cpp` generalized over the interfaces (WPM window, switch window, model call, logging format unchanged).
- `ml_wrapper.{h,cpp}` — moved or shared as-is; already portable (verified compiling with Apple clang).

**Linux stays on its existing code path.** Converging Linux onto `portable_engine` is optional, later, and only with owner approval plus identical integration/benchmark results.

### 2.4 Proposed layout
```
catra-project/
├── core/                       portable, no OS headers
├── platform/
│   ├── platform.h              interfaces
│   ├── macos/                  keystroke_cgevent.cpp, context_nsworkspace.mm, enumerator_libproc.cpp,
│   │                           actuator_setpriority.cpp, state_store_posix.cpp, main_macos.cpp
│   └── windows/                keystroke_llhook.cpp, context_foreground.cpp, enumerator_toolhelp.cpp,
│                               actuator_threadprio.cpp, state_store_win.cpp, main_windows.cpp
├── daemon/                     Linux (unchanged)
├── packaging/
│   ├── macos/                  com.catra.agent.plist, com.catra.helper.plist, entitlements
│   └── windows/                install.ps1, uninstall.ps1, service config
├── tests/
│   ├── core/                   portable unit tests (run on all OSes)
│   ├── macos/                  integration_test.sh, benchmark.sh (zsh/bash, macOS tools)
│   └── windows/                integration_test.ps1, benchmark.ps1
└── CMakeLists.txt              cross-platform build (Linux keeps build.sh too)
```
Note: `.gitignore` ignores `Makefile`, `*.cmake`, `CMakeFiles/`, `build/`. `CMakeLists.txt` is not ignored; do not add `*.cmake` helper files without adjusting `.gitignore`.

---

## 3. Phase 0 — Portable foundations (all platforms)

**Goal:** shared core + build system + CI, with zero behaviour change on Linux.

### Tasks
1. Add `core/rolling_window.h`, `core/hysteresis_policy.*`, `platform/platform.h`.
2. Add `CMakeLists.txt`:
   - target `catra_model` (C, from `daemon/main.c`), `catra_core` (C++20), and per-OS executables guarded by `if(APPLE)`, `if(WIN32)`, `if(UNIX AND NOT APPLE)`.
   - Linux target builds the **existing** `daemon/*.cpp` exactly as `build.sh` does. Keep `build.sh`.
   - Warnings: `-Wall -Wextra` (clang/gcc), `/W4` (MSVC).
3. Confirm the Treelite C predictor compiles under MSVC (`/std:c11`). It uses `expf`, `union`, and guards `__builtin_expect`; expected fine **[VERIFY]**.
4. Tests in `tests/core/` (plain `assert` style like the existing tests, or single-header doctest/Catch2 — owner decision D5):
   - `RollingWindow`: capacity, eviction, partial-fill sums.
   - `HysteresisPolicy`: table-driven — promote after exactly 2 warm ticks >15%; no action in 5–15% dead band; demote after exactly 3 cold/idle/<5% ticks; counters reset on branch change.
   - `FocusModel`: the prediction table from `PROJECT_CONTEXT.md` §3.6, plus idle-guard cases.
   - `StateCodec`: round-trip `id:token:original` lines; rejects malformed lines.
5. GitHub Actions matrix (`ubuntu-latest`, `macos-latest`, `windows-latest`): configure, build, run `tests/core` + model tests. No root/permission-dependent tests in CI.

### Acceptance
- CI green on all three OSes.
- On EC2: `bash build.sh` → `integration_test.sh` 15/15 and `benchmark.sh` ≈75% (Linux unchanged).

---

## 4. Phase 1 — macOS port

### 4.1 Spikes (do first, 1 small program each, record findings in this file)
| Spike | Question | Pass criterion |
|---|---|---|
| S1 | Can a listen-only `CGEventTap` count key-downs, skipping autorepeat (`kCGKeyboardEventAutorepeat`)? What permission prompt appears? | Count matches typed keys; repeats excluded |
| S2 | Run as `sudo` from Terminal with Terminal granted Input Monitoring — does the tap still work? (decides D1 for the MVP) **[VERIFY]** | Tap receives events while process is root |
| S3 | Does `proc_pidinfo(PROC_PIDTASKINFO)` return CPU time in Mach units on Apple Silicon? | Converted value matches `ps -o time` within 2% |
| S4 | Does `setpriority(PRIO_PROCESS, pid, -5)` on a busy process change its CPU share against equal-QoS noise? On P and E cores? | Measurable share increase (record the number) |
| S5 | Do `/dev/ttys*` atimes update on keyboard input like Linux pts? | atime changes within 1 s of typing |
| S6 | Does `NSWorkspace.frontmostApplication` + activation notifications work from a non-bundled CLI with a run loop? | Switch count increments on Cmd-Tab |

### 4.2 Design
- **Keystrokes:** `CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionListenOnly, CGEventMaskBit(kCGEventKeyDown), cb, ctx)` on a dedicated thread with its own `CFRunLoop`. Callback: if not autorepeat → `count.fetch_add(1)`. Re-enable on `kCGEventTapDisabledByTimeout` / `ByUserInput`. `running()` false if the tap cannot be created (permission denied) → log a clear message telling the user to grant Input Monitoring.
- **Test keystrokes:** reuse the Linux `KeyboardSensor` in FIFO mode (`/tmp/fake_keyboard` + `mock_keyboard.py`) — it already compiles on macOS with the stand-in `input_event`. Select with a CLI flag `--keys fifo:/tmp/fake_keyboard` vs `--keys system`.
- **Context:** two providers, combined:
  - *Terminal context* (parity with Linux): newest-atime `/dev/ttys*` (if S5 passes); roots = processes whose `e_tdev` matches; exclude shells/session leaders as on Linux (`zsh`, `bash`, `login`, `tmux`, `screen`, `sshd`).
  - *App context* (new): frontmost app PID; roots = that PID's descendants (covers IDEs and GUI build tools — closes a Linux limitation).
  - `take_switch_count()` = real app activations (S6) — feeds the model's `window_switches_per_min` with the signal it was designed for.
- **Enumeration:** `proc_listallpids` → `proc_pidinfo(PROC_PIDTBSDINFO)` for `pbi_ppid`, `pbi_pgid`, `e_tdev`, `e_tpgid`, `pbi_uid`, `pbi_comm`, start time; `PROC_PIDTASKINFO` for CPU. Units are **processes**.
- **Actuation:** `getpriority/setpriority(PRIO_PROCESS, pid, …)`; Tier 1 = −5 (keep Linux value unless S4 says otherwise; decision D2). Inherited boost: a child process forked from a boosted parent starts at −5 → baseline = parent's original (same rule as Linux, at process level).
- **State:** `/Library/Application Support/CATRA/state.dat`, dir 0700 root, file 0600, temp file + `rename()`; reuse the Linux file format with `pid:start_token:original`.
- **Supervision / privilege (D1):**
  - *MVP:* single binary run via `sudo` from a Terminal that has Input Monitoring (if S2 passes).
  - *Product:* `LaunchAgent` (user session: sensor, context, model) + `LaunchDaemon` privileged helper (enumeration + actuation + state) talking over a Unix domain socket in `/var/run/catra/` with peer-credential check (`getpeereid`). Registered via `SMAppService` (macOS 13+) inside a signed app bundle. Helper must restore all boosts if the agent disconnects for > N seconds.
- **Dev gotcha:** TCC permissions are tied to the binary's code signature. Ad-hoc rebuilds can invalidate the grant; sign dev builds with a stable identity or re-grant after rebuilds.

### 4.3 macOS testing plan
| Level | What | How | Where |
|---|---|---|---|
| Unit | core + model | `ctest` | CI + local |
| Component | keystroke tap | S1 harness: type 20 keys, expect 20 | local (needs permission) |
| Component | enumerator | spawn `yes`, find it by PID, CPU time rises ~1 s/s | CI-capable |
| Component | actuator | spawn child, boost to −5, restore, verify with `ps -o ni=` | local, sudo |
| Component | crash state | write state, `kill -9`, restart, child back to original | local, sudo |
| Integration | port of `tests/integration_test.sh` → `tests/macos/integration_test.sh` with the same 6 phases (terminal path, idle guard, ML path via FIFO mock, ML release, SIGKILL, SIGTERM) + new phase 7: *app path* (frontmost app's child boosted) | local, sudo, hands off keyboard |
| Benchmark | `tests/macos/benchmark.sh`: no `taskset` on macOS → start `ncpu` noise `yes` processes via `setsid`-equivalent (`nohup … &` with stdin from `/dev/null`, detached from the tty) plus 1 FG `yes` in the terminal; FG share vs fair share `1/(ncpu+1)`, CATRA off vs on. Read CPU with `ps -o time= -p PID`. Repeat 3×, report mean ± spread (P/E core placement adds noise) | local, sudo |

### 4.4 Acceptance (macOS)
- All component tests pass; integration 7 phases pass on Apple Silicon (and Intel if available).
- Benchmark shows a repeatable FG gain (record the number; there is no fixed target until S4 establishes what nice can do on XNU).
- Zero leftover boosts after: SIGTERM, SIGKILL + restart, agent crash (product mode), logout.

---

## 5. Phase 2 — Windows port

### 5.1 Spikes
| Spike | Question | Pass criterion |
|---|---|---|
| W1 | `WH_KEYBOARD_LL` hook in a user-session process: count `WM_KEYDOWN`/`WM_SYSKEYDOWN`, skip auto-repeat (track key state) | Count matches typed keys |
| W2 | Does an **elevated** process launched at logon (Task Scheduler "Run with highest privileges") receive hook events for the user's desktop? (decides D1 MVP) | Events arrive |
| W3 | `SetThreadPriority(THREAD_PRIORITY_ABOVE_NORMAL)` on another process's thread vs equal noise on one core (`SetProcessAffinityMask`) — measured share? Any starvation? | Record numbers; noise must still progress |
| W4 | Interaction with Windows foreground boost (`Win32PrioritySeparation`) — does baseline already favour the foreground window? | Record baseline FG share with FG focused vs unfocused |
| W5 | Is `GetForegroundWindow` → PID → descendants a good scope for Windows Terminal (`WindowsTerminal.exe` → `OpenConsole.exe` → `pwsh.exe` → `cl.exe`) and VS Code terminals? | Build tool processes found |
| W6 | Does security software flag the LL hook? Test with Defender default settings | No detection, or documented mitigation |

### 5.2 Design
- **Keystrokes:** `SetWindowsHookExW(WH_KEYBOARD_LL, …)` on a dedicated thread running a `GetMessage` loop. Hook proc must return within milliseconds (`LowLevelHooksTimeout`): only increment an atomic, then `CallNextHookEx`. Count key-down transitions only (keep a 256-bit "is down" set so held-key auto-repeat is ignored). Ignore `LLKHF_INJECTED` events in production; accept them only with `--accept-injected` (test mode).
- **Test keystrokes:** (a) named-pipe source `\\.\pipe\catra_fake_keyboard` fed by a `mock_keyboard.ps1` (same rate options as the Python mock), and/or (b) `SendInput` with `--accept-injected`.
- **Context:** `SetWinEventHook(EVENT_SYSTEM_FOREGROUND, …, WINEVENT_OUTOFCONTEXT)` → count real switches; foreground PID via `GetWindowThreadProcessId`. Roots = foreground PID; scope = descendants via Toolhelp parent links. **Parent PIDs can be stale on Windows** (parent exited and PID reused): only accept a parent link if `parent.creation_time <= child.creation_time`.
- **Exclusions:** `explorer.exe`, `WindowsTerminal.exe`, `OpenConsole.exe`, `conhost.exe`, `cmd.exe`, `powershell.exe`, `pwsh.exe`, `csrss.exe`, `dwm.exe`, plus any process that cannot be opened (protected/system).
- **Enumeration:** `CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS | TH32CS_SNAPTHREAD)`; per thread `OpenThread(THREAD_QUERY_LIMITED_INFORMATION | THREAD_SET_LIMITED_INFORMATION)`, `GetThreadTimes` (creation time = identity token; kernel+user = CPU). Units are **threads** (parity with Linux).
- **Actuation:** `GetThreadPriority` to record original; boost to `THREAD_PRIORITY_ABOVE_NORMAL` (D2); restore exact original. Re-open the thread and compare creation time before every change. Never use `REALTIME_PRIORITY_CLASS` or `THREAD_PRIORITY_TIME_CRITICAL`.
- **Inherited boost:** new threads inherit the *process* class, not thread offsets, so the Linux inherited-boost case mostly disappears for threads; keep the check for process-class mode if D2 picks `SetPriorityClass`.
- **State:** `%ProgramData%\CATRA\state.dat` written via temp file + `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`; directory ACL SYSTEM + Administrators only; on load, reject if the file's owner/ACL is not as expected. Format `tid:creation_filetime:original`.
- **Supervision / privilege (D1):**
  - *MVP:* one elevated process started at logon by Task Scheduler (if W2 passes), restart-on-failure configured in the task.
  - *Product:* Windows Service (Session 0, LocalSystem: enumeration, actuation, state) + per-user agent (hooks, context, model) connected by a named pipe with an ACL restricted to the interactive user + SYSTEM. Service recovery actions: restart after 5 s. Service restores everything if the agent disconnects.
- **Shutdown:** handle `CTRL_CLOSE_EVENT`/`CTRL_SHUTDOWN_EVENT` (console) and `SERVICE_CONTROL_STOP`/`PRESHUTDOWN` (service) → `restore_all()`.
- **Build:** MSVC 2022, `/std:c++20 /W4 /permissive-`; model as C (`/std:c11`). Link `user32.lib`.

### 5.3 Windows testing plan
| Level | What | How | Where |
|---|---|---|---|
| Unit | core + model | `ctest` | CI + local |
| Component | LL hook counter | type 20 keys → 20; hold a key → 1 | local |
| Component | enumerator | start `ping -t localhost` or a CPU burner, find its threads, CPU rises | CI-capable |
| Component | actuator | burner child → boost → `Get-Process` / thread priority check → restore | local, admin |
| Component | crash state | boost, `Stop-Process -Force` the daemon, restart, priorities restored | local, admin |
| Integration | `tests/windows/integration_test.ps1`: same 6 phases using the pipe mock; "terminal cold" = foreground switched away for > 30 s; plus phase 7 *foreground app path* | local, admin, hands off |
| Benchmark | `tests/windows/benchmark.ps1`: FG and NOISE burners pinned to CPU 0 via `ProcessorAffinity`; measure `TotalProcessorTime` deltas over 15 s; CATRA off vs on; run with FG window focused and unfocused (W4) | local, admin |

Burner for tests: a tiny `burn.exe` built from `tests/windows/burn.cpp` (`for(;;){}` per thread), so thread counts are controlled.

### 5.4 Acceptance (Windows)
- Component + integration tests pass on Windows 11 (and Windows 10 if supported).
- Benchmark shows FG gain **and** NOISE never drops to 0% over the window (no starvation).
- Zero leftover boosts after stop, forced kill + restart, logoff, reboot.
- No antivirus detection with default Defender settings (W6).

---

## 6. Phase 3 — Model upgrade for real window switches

Both ports can measure **real** window/app switches, which is what the model's second feature was designed for. The Linux TTY proxy is much weaker.

1. Retrain with idle/low-activity Relax samples first (Linux plan, `PROJECT_CONTEXT.md` §9.1). Remove the idle guard once the model handles (0,0) correctly.
2. Add an opt-in, local-only telemetry log (`--record-features out.csv`) on all platforms: per tick `wpm, switches_per_min, user_label?` — never key contents. Owner decides consent model (D4).
3. Decide per-platform models vs one model with a `platform` feature (D3). Keep the feature order contract in `ml_wrapper` and `train_and_export.py` in sync.
4. Validation: hold-out accuracy on real data ≥ synthetic-data accuracy; prediction table checked into `tests/core/model_expectations.txt` and asserted in CI.

---

## 7. Phase 4 — Packaging and distribution

| Platform | Deliverable | Notes |
|---|---|---|
| Linux | `.deb`/`.rpm` or install script + `catra.service` | keyboard device auto-detect via `/dev/input/by-id/*-event-kbd` |
| macOS | signed + notarized `.app` with `SMAppService` helper, or `.pkg` | Developer ID required; document Input Monitoring prompt |
| Windows | signed MSI/MSIX or `install.ps1` registering service + logon task | code signing strongly recommended (hooks + elevation) |

Uninstall must call `restore_all()` and delete state files on every platform.

---

## 8. Cross-platform testing strategy (summary)

| Test type | Linux | macOS | Windows | In CI? |
|---|---|---|---|---|
| Core unit tests (window, hysteresis, codec) | yes | yes | yes | yes |
| Model tests (prediction table) | yes | yes | yes | yes |
| Keystroke source component | FIFO | FIFO + event tap | pipe + LL hook | mock only |
| Enumerator component | yes | yes | yes | yes (no root needed) |
| Actuator component | root | sudo | admin | no |
| Crash recovery | root | sudo | admin | no |
| Integration (6–7 phases) | `integration_test.sh` | `tests/macos/integration_test.sh` | `integration_test.ps1` | no (needs interactive session) |
| Benchmark | `benchmark.sh` | macOS benchmark | `benchmark.ps1` | no |

Rules for every test:
- Each integration/benchmark script has a preflight (privileges, binary present, no running CATRA, no stray burners) and a cleanup trap that restores priorities even on failure.
- Every boost assertion is paired with a restore assertion.
- Results (numbers, OS version, CPU model, core count) are appended to a results table in `PROJECT_CONTEXT.md` §6 after each real run.

---

## 9. Decisions needed from the owner

| ID | Decision | Options | Recommendation |
|---|---|---|---|
| D1 | Privilege model per OS | (a) single elevated process in user session (MVP) · (b) agent + privileged helper/service (product) | (a) for the first working port, then (b) |
| D2 | "Tier 1" mapping | macOS: nice −5 vs other · Windows: `THREAD_PRIORITY_ABOVE_NORMAL` (+1) vs `HIGHEST` (+2) vs process `ABOVE_NORMAL_PRIORITY_CLASS` | macOS −5; Windows thread +1 (strict-priority scheduler makes +1 already strong) |
| D3 | Model strategy | one shared model · per-platform models | shared until real data shows platform differences |
| D4 | Real telemetry collection | none · opt-in local CSV · opt-in upload | opt-in local CSV only |
| D5 | Unit-test framework | plain asserts (current style) · doctest/Catch2 single header | doctest (single header, no build deps) |
| D6 | Converge Linux onto `portable_engine` later? | never · after parity proven | only after identical Linux test/benchmark results, with owner sign-off |
| D7 | Optional demotion of background load (macOS `PRIO_DARWIN_BG`, Windows EcoQoS) | out of scope · opt-in feature | out of scope for the ports; revisit later (it is a new mechanic, not a port) |

---

## 10. Risks

| Risk | Platform | Mitigation |
|---|---|---|
| Keyboard hook flagged as keylogger | Windows, macOS | count-only code, code signing, clear permission text, document behaviour |
| TCC grant lost on every rebuild | macOS | stable signing identity for dev builds |
| `nice` has little effect under QoS / P-E cores | macOS | spike S4 before building; consider QoS-aware approach as a separate decision |
| Strict-priority starvation of background threads | Windows | use +1 only, verify NOISE > 0% in benchmark, never use realtime classes |
| Stale parent PIDs build wrong process trees | Windows | creation-time ordering check on every parent link |
| Session 0 isolation blocks hooks in services | Windows | agent/service split (D1b) |
| Agent dies while helper keeps boosts | macOS, Windows (split design) | helper restores everything on agent disconnect timeout |
| Linux regression from shared-code changes | all | Linux code untouched; mandatory EC2 integration + benchmark gate per phase |

---

## 11. Suggested execution order for an agent

1. Read `PROJECT_CONTEXT.md`, then this file.
2. Phase 0 (core + CMake + CI). Stop and report; get owner OK.
3. macOS spikes S1–S6 → write findings into §4.1 → ask owner for D1/D2.
4. macOS MVP → component tests → integration → benchmark → record results.
5. Windows spikes W1–W6 → findings into §5.1 → owner D1/D2.
6. Windows MVP → tests → results.
7. Phase 3 model work, then Phase 4 packaging.

At the end of each step: update this file's status lines and `PROJECT_CONTEXT.md` §6/§8 so the next agent starts from the truth.
