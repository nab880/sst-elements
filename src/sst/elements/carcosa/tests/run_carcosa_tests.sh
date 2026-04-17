#!/bin/bash
set +e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

TIMEOUT="${CARCOSA_TEST_TIMEOUT:-600}"
SST="${SST_BIN:-sst}"

if ! command -v "$SST" >/dev/null 2>&1; then
    echo "error: '$SST' not found on PATH; set SST_BIN or PATH." >&2
    exit 2
fi

CORE_TESTS=(
    "testCorruptMemBasic.py"
    "testCorruptMemDouble.py"
    "testCorruptMemDoubleOverlap.py"
    "testRandomDrop.py"
    "testRandomFlip.py"
    "testHaliMemH.py"
    "testHaliBacking.py"
    "testHaliPM.py"
    "testManagerLogic.py"
    "testDynamicPM.py"
    "testCarcosaPingPong.py"
)

STUCKAT_TESTS=(
    "testStuckAtBasic.py"
    "testStuckAtMultiple.py"
    "testStuckAtOverlap.py"
    "testStuckAtSameByte.py"
)

OUTDIR="$(mktemp -d -t carcosa-tests-XXXXXX)"
PASSED=()
FAILED=()
TIMEDOUT=()
SKIPPED=()

run_one() {
    local testfile="$1"
    local log="$OUTDIR/${testfile%.py}.log"

    ("$SST" "$testfile" > "$log" 2>&1) &
    local pid=$!
    (
        sleep "$TIMEOUT"
        if kill -0 "$pid" 2>/dev/null; then
            kill -9 "$pid" 2>/dev/null
            echo "__TIMEOUT__" >> "$log"
        fi
    ) &
    local watcher=$!
    wait "$pid" 2>/dev/null
    local rc=$?
    kill -9 "$watcher" 2>/dev/null
    wait "$watcher" 2>/dev/null

    if grep -q "__TIMEOUT__" "$log"; then
        printf "  TIMEOUT  %s  (>%ds)\n" "$testfile" "$TIMEOUT"
        TIMEDOUT+=("$testfile")
        return 1
    fi
    if [ "$rc" -ne 0 ]; then
        printf "  FAIL     %s  (exit=%d)\n" "$testfile" "$rc"
        grep -E "FATAL|Error|error|ABORT|fatal|terminate|assert" "$log" | head -3 | sed 's/^/           /'
        FAILED+=("$testfile")
        return 1
    fi
    if ! grep -q "Simulation is complete" "$log"; then
        printf "  FAIL     %s  (no 'Simulation is complete' in stdout)\n" "$testfile"
        FAILED+=("$testfile")
        return 1
    fi
    printf "  PASS     %s  (%s)\n" "$testfile" \
        "$(grep 'Simulation is complete' "$log" | tail -1 | sed 's/^Simulation is complete, //')"
    PASSED+=("$testfile")
    return 0
}

echo "Carcosa local test runner"
echo "  sst binary  : $(command -v "$SST")"
echo "  per-test TO : ${TIMEOUT}s"
echo "  log dir     : $OUTDIR"
echo

echo "Core tests:"
for t in "${CORE_TESTS[@]}"; do
    run_one "$t"
done

echo
if [ "${RUN_STUCKAT:-0}" = "1" ]; then
    echo "StuckAt tests (RUN_STUCKAT=1):"
    for t in "${STUCKAT_TESTS[@]}"; do
        run_one "$t"
    done
else
    echo "StuckAt tests: SKIPPED (set RUN_STUCKAT=1 to run)"
    for t in "${STUCKAT_TESTS[@]}"; do
        SKIPPED+=("$t")
    done
fi

echo
echo "Summary:"
printf "  passed  : %d\n" "${#PASSED[@]}"
printf "  failed  : %d" "${#FAILED[@]}"
if [ "${#FAILED[@]}" -gt 0 ]; then
    printf " (%s)" "$(IFS=,; echo "${FAILED[*]}")"
fi
echo
printf "  timeout : %d" "${#TIMEDOUT[@]}"
if [ "${#TIMEDOUT[@]}" -gt 0 ]; then
    printf " (%s)" "$(IFS=,; echo "${TIMEDOUT[*]}")"
fi
echo
printf "  skipped : %d\n" "${#SKIPPED[@]}"

if [ "${#FAILED[@]}" -gt 0 ] || [ "${#TIMEDOUT[@]}" -gt 0 ]; then
    exit 1
fi
exit 0
