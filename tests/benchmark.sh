#!/usr/bin/env bash
# CATRA CPU-share benchmark (Linux, root).
#
#   cd CATRA && sudo bash tests/benchmark.sh
#
# Question answered: when the user's workload competes for a CPU core, does CATRA's
# -5 boost actually give it more CPU time?
#
# Setup per scenario (both pinned to the SAME core so they must compete):
#   FG    : 'yes' started from this terminal  -> in CATRA's scope (the "user's compile")
#   NOISE : 'yes' started via setsid, no tty  -> outside CATRA's scope (background load)
#
# Each scenario measures FG's share of the core for MEASURE_SECS:
#   baseline  -> CATRA not running (both nice 0)
#   catra     -> CATRA running, mock typing keeps the ML model at FOCUS, FG boosted to -5
#
# Run with kernel.sched_autogroup_enabled = 1 and = 0 (original value restored on exit).
# Theory (CFS weights): nice -5 vs 0 = 3121 / (3121 + 1024) = 75.3% for FG,
# but only when both tasks compete in the same scheduling group (autogroup off).
#
# Hands off the keyboard for ~2.5 minutes.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/daemon/catra"
MOCK="$ROOT/tests/mock_keyboard.py"
FIFO="/tmp/fake_keyboard"
LOG="$(mktemp /tmp/catra_bench_log.XXXXXX)"
AG_PATH="/proc/sys/kernel/sched_autogroup_enabled"
MEASURE_SECS=15
BENCH_CORE=0

FG_PID=""; NOISE_PID=""; DAEMON_PID=""; MOCK_PID=""
ORIG_AG=""
declare -a RESULTS=()

info() { printf '\033[36m[%3ss] %s\033[0m\n' "$SECONDS" "$*"; }
red()  { printf '\033[31m%s\033[0m\n' "$*"; }

nice_of() { ps -o ni= -p "$1" 2>/dev/null | tr -d ' '; }

# utime + stime (clock ticks) for a PID; parse after the last ')' to be comm-safe.
cpu_ticks() {
    local line; line="$(cat "/proc/$1/stat" 2>/dev/null)" || { echo 0; return; }
    echo "${line##*) }" | awk '{print $12 + $13}'
}

# Prints FG share of the core (percent, one decimal) over MEASURE_SECS.
measure_share() {
    local f0 n0 f1 n1
    f0=$(cpu_ticks "$FG_PID"); n0=$(cpu_ticks "$NOISE_PID")
    sleep "$MEASURE_SECS"
    f1=$(cpu_ticks "$FG_PID"); n1=$(cpu_ticks "$NOISE_PID")
    awk -v df=$((f1 - f0)) -v dn=$((n1 - n0)) \
        'BEGIN { t = df + dn; if (t <= 0) print "0.0"; else printf "%.1f", 100.0 * df / t }'
}

# Run helper processes (daemon, mock) off the benchmark core when possible.
if (( $(nproc) > 1 )); then HELPER=(taskset -c 1); else HELPER=(); fi

start_burners() {
    taskset -c "$BENCH_CORE" yes >/dev/null &
    FG_PID=$!
    # New session, no controlling tty: invisible to CATRA, own autogroup when enabled.
    setsid taskset -c "$BENCH_CORE" yes </dev/null >/dev/null 2>&1 &
    sleep 0.5
    # setsid may fork, so $! is unreliable: NOISE is the 'yes' that is not FG.
    NOISE_PID=$(pgrep -x yes | grep -vx "$FG_PID" | tail -n 1)
    if [[ -z "$NOISE_PID" ]]; then
        red "Could not locate NOISE process"; exit 1
    fi
    sleep 2.5 # settle
}

stop_burners() {
    for p in "$FG_PID" "$NOISE_PID"; do
        [[ -n "$p" ]] && kill "$p" 2>/dev/null && wait "$p" 2>/dev/null
    done
    FG_PID=""; NOISE_PID=""
}

start_daemon() {
    "${HELPER[@]}" "$BIN" "$FIFO" >>"$LOG" 2>&1 &
    DAEMON_PID=$!
    sleep 1
    kill -0 "$DAEMON_PID" 2>/dev/null || { red "Daemon failed to start:"; tail "$LOG"; exit 1; }
}

stop_daemon() {
    [[ -n "$DAEMON_PID" ]] && kill -TERM "$DAEMON_PID" 2>/dev/null && wait "$DAEMON_PID" 2>/dev/null
    DAEMON_PID=""
}

start_typing() {
    "${HELPER[@]}" python3 "$MOCK" --fifo "$FIFO" --wpm 80 --duration 120 --quiet >/dev/null 2>&1 &
    MOCK_PID=$!
}

stop_typing() {
    [[ -n "$MOCK_PID" ]] && kill "$MOCK_PID" 2>/dev/null && wait "$MOCK_PID" 2>/dev/null
    MOCK_PID=""
}

cleanup() {
    stop_typing
    stop_daemon
    stop_burners
    [[ -n "$ORIG_AG" ]] && echo "$ORIG_AG" >"$AG_PATH" 2>/dev/null
}
trap cleanup EXIT INT TERM

run_scenario() { # <autogroup 0|1>
    local ag=$1 base boost fg_nice
    echo "$ag" >"$AG_PATH"
    info "=== autogroup=$ag ==="

    start_burners
    info "FG PID=$FG_PID (nice $(nice_of "$FG_PID")), NOISE PID=$NOISE_PID, both on core $BENCH_CORE"

    info "Baseline: CATRA off, measuring ${MEASURE_SECS}s..."
    base=$(measure_share)
    info "  FG share = ${base}%"

    info "CATRA on + mock typing at 80 WPM, waiting for boost..."
    start_daemon
    start_typing
    for ((i = 0; i < 40; i++)); do
        [[ "$(nice_of "$FG_PID")" == "-5" ]] && break
        sleep 1
    done
    fg_nice="$(nice_of "$FG_PID")"
    if [[ "$fg_nice" != "-5" ]]; then
        red "  FG was not boosted (nice=$fg_nice) - see $LOG"
        boost="n/a"
    else
        info "  FG boosted to -5 after ${i}s, measuring ${MEASURE_SECS}s..."
        boost=$(measure_share)
        info "  FG share = ${boost}%"
    fi

    stop_typing
    stop_daemon
    stop_burners
    RESULTS+=("$ag|$base|$boost|$fg_nice")
}

# ---------------------------------------------------------------- preflight
[[ $EUID -eq 0 ]] || { red "Run as root: sudo bash tests/benchmark.sh"; exit 1; }
[[ -x "$BIN" ]]   || { red "Binary not found: $BIN (run: bash build.sh)"; exit 1; }
command -v taskset >/dev/null || { red "taskset missing: sudo dnf install -y util-linux"; exit 1; }
if pgrep -x catra >/dev/null || systemctl is-active --quiet catra 2>/dev/null; then
    red "CATRA already running; stop it first"; exit 1
fi
if pgrep -x yes >/dev/null; then
    red "Stray 'yes' processes running (pkill -x yes); they would skew results"; exit 1
fi
[[ -r "$AG_PATH" ]] || { red "$AG_PATH not available on this kernel"; exit 1; }
[[ -p "$FIFO" ]] || mkfifo -m 0622 "$FIFO"

ORIG_AG="$(cat "$AG_PATH")"
info "vCPUs=$(nproc), original autogroup=$ORIG_AG, log=$LOG"
info "Hands off the keyboard for ~2.5 minutes."

run_scenario 1
run_scenario 0

# ---------------------------------------------------------------- report
echo
printf '%-10s | %-16s | %-16s | %-10s | %s\n' "autogroup" "FG share (off)" "FG share (CATRA)" "speedup" "FG nice"
printf '%s\n' "-----------+------------------+------------------+------------+--------"
for row in "${RESULTS[@]}"; do
    IFS='|' read -r ag base boost nicev <<<"$row"
    if [[ "$boost" == "n/a" ]]; then
        speed="n/a"
    else
        speed=$(awk -v b="$base" -v x="$boost" 'BEGIN { if (b > 0) printf "%.2fx", x / b; else print "n/a" }')
    fi
    printf '%-10s | %15s%% | %15s%% | %-10s | %s\n' "$ag" "$base" "$boost" "$speed" "$nicev"
done
echo
echo "Theory: autogroup=0 -> ~75.3% with CATRA (CFS weight 3121 vs 1024); autogroup=1 -> ~50% (separate groups)."
echo "Autogroup restored to $ORIG_AG. Log: $LOG"
