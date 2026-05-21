#!/usr/bin/env bash
# monitor_ecc_pipeline.sh — poll run_all_ecc output; macOS notify on success or failure.
#
# Usage:
#   monitor_ecc_pipeline.sh <OUT_ROOT> [poll_seconds]
#
# Alerts:
#   - Success: pipeline.log contains "All done in"
#   - Failure: run_all_ecc exits non-zero, or new "-> exit N" (N!=0) in pipeline.log

set -u

OUT_ROOT="${1:?OUT_ROOT required}"
POLL="${2:-60}"
PIPELOG="$OUT_ROOT/pipeline.log"
SWEEP_PID_FILE="$OUT_ROOT/.monitor_sweep_pid"
STATE_FILE="$OUT_ROOT/.monitor_state"

notify() {
    local title="$1"
    local msg="$2"
    echo "[$(date +%H:%M:%S)] ALERT: $title — $msg"
    if command -v osascript >/dev/null 2>&1; then
        osascript -e "display notification \"${msg}\" with title \"${title}\" sound name \"Glass\"" 2>/dev/null || true
    fi
    # Also append for log tailing / CI
    echo "$(date -Iseconds)	$title	$msg" >> "$OUT_ROOT/monitor_alerts.log"
}

count_logs() {
    local d="$OUT_ROOT/ecc_sweep_out"
    [ -d "$d" ] || { echo 0; return; }
    ls "$d"/*.log 2>/dev/null | wc -l | tr -d ' '
}

last_ber_line() {
    [ -f "$PIPELOG" ] || return 0
    grep '^=== ber=' "$PIPELOG" 2>/dev/null | tail -1 || true
}

# Track last seen failure line count to detect new failures
fail_count() {
    [ -f "$PIPELOG" ] || { echo 0; return; }
    grep -E '-> exit [1-9]|-> exit [0-9]{2,}' "$PIPELOG" 2>/dev/null | wc -l | tr -d ' '
}

echo "Monitoring $OUT_ROOT (poll=${POLL}s)"
echo "  pipeline.log: $PIPELOG"
echo "  alerts log:   $OUT_ROOT/monitor_alerts.log"

# Remember pipeline parent pid if run_all_ecc still running at start
MONITOR_T0=$SECONDS
LAST_FAIL=$(fail_count)
RUNALL_PID=""

while true; do
  if [ -f "$OUT_ROOT/.run_all_ecc.pid" ]; then
    RUNALL_PID=$(cat "$OUT_ROOT/.run_all_ecc.pid" 2>/dev/null || true)
  fi
  if [ -z "$RUNALL_PID" ]; then
    RUNALL_PID=$(pgrep -f "bash run_all_ecc.sh" 2>/dev/null | head -1 || true)
  fi

  if [ -f "$PIPELOG" ] && grep -q 'All done in' "$PIPELOG" 2>/dev/null; then
    N=$(count_logs)
    notify "VLA-ECC sweep complete" "Finished at $OUT_ROOT ($N sweep logs). See analysis/ under ecc_sweep_out."
    echo "done" > "$STATE_FILE"
    exit 0
  fi

  FC=$(fail_count)
  if [ "$FC" -gt "$LAST_FAIL" ]; then
    NEW=$(grep -E '-> exit [1-9]|-> exit [0-9]{2,}' "$PIPELOG" | tail -1)
    notify "VLA-ECC sweep failure" "New failed cell: ${NEW:-unknown}. OUT_ROOT=$OUT_ROOT"
    LAST_FAIL=$FC
    echo "failed_cell" >> "$STATE_FILE"
  fi

  if [ -n "$RUNALL_PID" ] && ! kill -0 "$RUNALL_PID" 2>/dev/null; then
    # Process ended without success banner
    if [ -f "$PIPELOG" ] && ! grep -q 'All done in' "$PIPELOG" 2>/dev/null; then
      N=$(count_logs)
      LAST=$(last_ber_line)
      notify "VLA-ECC pipeline stopped" "run_all_ecc exited early ($N logs). Last: ${LAST:-unknown}"
      echo "aborted" > "$STATE_FILE"
      exit 1
    fi
  fi

  # Progress heartbeat every 30 min (optional quiet log, not a user alert)
  EL=$((SECONDS - MONITOR_T0))
  if [ $((EL % 1800)) -lt "$POLL" ] && [ "$EL" -ge 1800 ]; then
    N=$(count_logs)
    echo "[$(date +%H:%M:%S)] progress: $N logs, $(last_ber_line)"
  fi

  sleep "$POLL"
done
