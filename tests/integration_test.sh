#!/usr/bin/env bash
# CATRA end-to-end integration test (Linux, root).
#
#   cd catra-project && sudo bash tests/integration_test.sh
#
# DO NOT TYPE in any terminal while it runs (~4 min): the AnchorEngine targets the
# /dev/pts with the newest atime, and keystrokes elsewhere would steal the target.
#
# Phases
#   1. TTY path      : terminal just touched (atime warm)          -> burner promoted to -5
#   2. Idle guard    : terminal cold + 0 WPM (ML relax)            -> burner demoted to 0
#   3. ML path       : terminal cold + mock typing 80 WPM          -> promoted, ml_signal=FOCUS, warm=no
#   4. ML release    : typing stops, WPM window decays              -> demoted
#   5. SIGKILL       : kill -9 while boosted, restart daemon        -> crash recovery restores 0
#   6. SIGTERM       : graceful stop while boosted                  -> restore_all, state file removed

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/daemon/catra"
MOCK="$ROOT/tests/mock_keyboard.py"
FIFO="/tmp/fake_keyboard"
STATE="/run/catra/state.dat"
LOG="$(mktemp /tmp/catra_it_log.XXXXXX)"

DAEMON_PID=""; BURNER_PID=""; MOCK_PID=""
PASS=0; FAIL=0

green() { printf '\033[32m%s\033[0m\n' "$*"; }
red()   { printf '\033[31m%s\033[0m\n' "$*"; }
info()  { printf '\033[36m[%3ss] %s\033[0m\n' "$SECONDS" "$*"; }

pass() { green "  [PASS] $*"; PASS=$((PASS + 1)); }
fail() { red   "  [FAIL] $*"; FAIL=$((FAIL + 1)); }

nice_of() { ps -o ni= -p "$1" 2>/dev/null | tr -d ' '; }

# wait_nice <pid> <expected> <timeout_s> <label>
wait_nice() {
    local pid=$1 want=$2 timeout=$3 label=$4 cur=""
    for ((i = 0; i < timeout; i++)); do
        cur="$(nice_of "$pid")"
        if [[ "$cur" == "$want" ]]; then
            pass "$label (nice=$cur after ${i}s)"
            return 0
        fi
        sleep 1
    done
    fail "$label (expected nice=$want, got '${cur}' after ${timeout}s)"
    return 1
}

start_daemon() {
    "$BIN" "$FIFO" >>"$LOG" 2>&1 &
    DAEMON_PID=$!
    sleep 1
    if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
        red "Daemon failed to start. Log:"; cat "$LOG"; exit 1
    fi
}

start_typing() { # <wpm> <duration_s>
    python3 "$MOCK" --fifo "$FIFO" --wpm "$1" --duration "$2" --quiet >/dev/null 2>&1 &
    MOCK_PID=$!
}

stop_typing() {
    [[ -n "$MOCK_PID" ]] && kill "$MOCK_PID" 2>/dev/null && wait "$MOCK_PID" 2>/dev/null
    MOCK_PID=""
}

cleanup() {
    stop_typing
    [[ -n "$DAEMON_PID" ]] && kill -TERM "$DAEMON_PID" 2>/dev/null && wait "$DAEMON_PID" 2>/dev/null
    if [[ -n "$BURNER_PID" ]]; then
        renice -n 0 -p "$BURNER_PID" >/dev/null 2>&1
        kill "$BURNER_PID" 2>/dev/null
    fi
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------- preflight
[[ $EUID -eq 0 ]] || { red "Run as root: sudo bash tests/integration_test.sh"; exit 1; }
[[ -x "$BIN" ]]   || { red "Binary not found: $BIN (build it first)"; exit 1; }
command -v python3 >/dev/null || { red "python3 required"; exit 1; }

if systemctl is-active --quiet catra 2>/dev/null; then
    red "catra.service is running; stop it first (sudo systemctl stop catra)"; exit 1
fi
if pgrep -x catra >/dev/null; then
    red "Another catra process is running; stop it first"; exit 1
fi

[[ -p "$FIFO" ]] || mkfifo -m 0622 "$FIFO"
rm -f "$STATE"

info "Log file: $LOG"
info "Hands off the keyboard for ~4 minutes."

# ---------------------------------------------------------------- Phase 1: TTY path
info "Phase 1: TTY-activity path (terminal was just used -> atime warm)"
yes >/dev/null &
BURNER_PID=$!
info "CPU burner 'yes' PID=$BURNER_PID, baseline nice=$(nice_of "$BURNER_PID")"
start_daemon
info "Daemon PID=$DAEMON_PID"
wait_nice "$BURNER_PID" -5 20 "Burner promoted while terminal is warm"

# ---------------------------------------------------------------- Phase 2: idle guard
info "Phase 2: terminal goes cold, 0 WPM -> ML must say RELAX (idle guard)"
wait_nice "$BURNER_PID" 0 60 "Burner demoted once terminal cold and no typing"
if tail -n 5 "$LOG" | grep -q "State: RELAX"; then
    pass "Daemon reports RELAX while idle"
else
    fail "Daemon not reporting RELAX while idle"
fi

# ---------------------------------------------------------------- Phase 3: ML path
info "Phase 3: ML path - mock typing at 80 WPM while terminal stays cold"
MARK=$(wc -l <"$LOG")
start_typing 80 40
if wait_nice "$BURNER_PID" -5 35 "Burner promoted by ML focus signal"; then
    sleep 3 # let buffered engine lines flush
    PROMO_STATE=$(tail -n +"$MARK" "$LOG" | awk -v pid="$BURNER_PID" '
        /\[AnchorEngine\] State:/ { last = $0 }
        $0 ~ "Promoted TID " pid " " { print last; exit }')
    echo "    sweep at promotion: $PROMO_STATE"
    if [[ "$PROMO_STATE" == *"warm=no"* && "$PROMO_STATE" == *"ml_signal=FOCUS"* ]]; then
        pass "Promotion was driven by the ML model (TTY cold, ml_signal=FOCUS)"
    else
        fail "Promotion was not attributable to ML alone"
    fi
fi
tail -n +"$MARK" "$LOG" | grep "\[CATRA\] WPM" | tail -n 3 | sed 's/^/    /'

# ---------------------------------------------------------------- Phase 4: ML release
info "Phase 4: typing stops -> WPM decays -> RELAX -> demotion"
wait "$MOCK_PID" 2>/dev/null; MOCK_PID=""
wait_nice "$BURNER_PID" 0 60 "Burner demoted after typing stopped"

# ---------------------------------------------------------------- Phase 5: SIGKILL
info "Phase 5: SIGKILL crash recovery"
start_typing 80 60
if wait_nice "$BURNER_PID" -5 35 "Burner boosted before crash"; then
    kill -9 "$DAEMON_PID"; wait "$DAEMON_PID" 2>/dev/null; DAEMON_PID=""
    stop_typing
    if [[ "$(nice_of "$BURNER_PID")" == "-5" ]]; then
        pass "Boost left behind by killed daemon (expected)"
    else
        fail "Burner not at -5 after SIGKILL"
    fi
    if [[ -f "$STATE" ]]; then
        pass "State file present: $(cat "$STATE")"
    else
        fail "State file $STATE missing after SIGKILL"
    fi
    MARK=$(wc -l <"$LOG")
    start_daemon
    sleep 2
    wait_nice "$BURNER_PID" 0 5 "Restarted daemon restored burner priority"
    if tail -n +"$MARK" "$LOG" | grep -q "Crash Recovery: Restored TID $BURNER_PID"; then
        pass "Crash Recovery log line present"
    else
        fail "No Crash Recovery log line for TID $BURNER_PID"
    fi
fi

# ---------------------------------------------------------------- Phase 6: SIGTERM
info "Phase 6: graceful SIGTERM while boosted"
start_typing 80 60
if wait_nice "$BURNER_PID" -5 35 "Burner boosted before SIGTERM"; then
    kill -TERM "$DAEMON_PID"; wait "$DAEMON_PID" 2>/dev/null; DAEMON_PID=""
    stop_typing
    wait_nice "$BURNER_PID" 0 3 "SIGTERM restored burner priority"
    if [[ ! -f "$STATE" ]]; then pass "State file removed on clean shutdown"; else fail "State file still present"; fi
    if grep -q "Daemon shutdown complete" "$LOG"; then pass "Clean shutdown logged"; else fail "No shutdown log line"; fi
fi

# ---------------------------------------------------------------- summary
echo
if (( FAIL == 0 )); then
    green "ALL $PASS CHECKS PASSED  (log: $LOG)"
else
    red "$FAIL FAILED / $PASS passed  (log: $LOG)"
fi
(( FAIL == 0 ))
