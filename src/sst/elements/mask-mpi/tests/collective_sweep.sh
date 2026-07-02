#!/usr/bin/env bash
#
# Correctness + timing comparison for user-selectable sumi all-reduce
# algorithms, driven through mask-mpi -- so it needs only `sst` (no hgcc, no
# MVAPICH2). This is the mask-mpi analog of the Tier 2 correctness matrix and
# comparison sweep documented in the user-collectives plan.
#
# For each (algorithm, rank count) it runs the self-checking test_allreduce.py:
# the app asserts the known sum n(n+1)/2, so a wrong DAG fails the assert (or
# deadlocks, caught by the engine); the simulator's reported time is recorded.
# "PASS everywhere" is the diff against the default -- a selected algorithm that
# disagrees fails the assert.
#
# Usage:
#   ./collective_sweep.sh
#   ALGS="ring recdouble myring" NRANKS_LIST="2 4 7 8 16" ./collective_sweep.sh
#   COLLECTIVE_PLUGINS=/abs/libmycoll.so ALGS="myring" ./collective_sweep.sh   # Linux
#
# Env:
#   ALGS         algorithm names (default "ring recdouble"); the empty default
#                is always included, shown as "<default>".
#   NRANKS_LIST  rank counts (default "2 4 7 8 16")
#   SST          path to the sst binary (default: sst on PATH)
#   OUT          CSV output path (default collective_sweep.csv here)

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"
export PYTHONPATH="$SCRIPT_DIR:${PYTHONPATH:-}"

SST="${SST:-sst}"
ALGS="${ALGS:-ring recdouble}"
NRANKS_LIST="${NRANKS_LIST:-2 4 7 8 16}"
OUT="${OUT:-$SCRIPT_DIR/collective_sweep.csv}"

echo "algorithm,nranks,status,time_s" > "$OUT"

# Parse "simulated time: 2.111 us" -> seconds (portable: no gawk-only match()).
to_seconds() {
  local line val unit f
  line=$(grep -oE "simulated time: [0-9.eE+-]+ [a-z]+" | tail -1)
  [[ -z "$line" ]] && return
  val=${line#simulated time: }; unit=${val##* }; val=${val% *}
  case "$unit" in
    s) f=1 ;; ms) f=0.001 ;; us) f=0.000001 ;;
    ns) f=0.000000001 ;; ps) f=0.000000000001 ;; *) return ;;
  esac
  awk -v v="$val" -v f="$f" 'BEGIN{printf "%.9e", v*f}'
}

run_one() {  # $1=nranks $2=label $3=ALG-value
  local n="$1" label="$2" alg="$3" out secs status
  out="$(NRANKS="$n" ALG="$alg" "$SST" test_allreduce.py 2>&1)"
  if grep -q "PASS: allreduce" <<<"$out"; then status=PASS; else status=FAIL; fi
  secs="$(printf '%s\n' "$out" | to_seconds)"
  printf "  n=%-3s %-12s %s  %s\n" "$n" "$label" "$status" "${secs:-no-time}"
  echo "$label,$n,$status,${secs:-}" >> "$OUT"
  [[ "$status" == PASS ]]
}

fails=0
echo "all-reduce comparison via mask-mpi (each cell must PASS):"
for n in $NRANKS_LIST; do
  run_one "$n" "<default>" "" || fails=$((fails+1))
  for alg in $ALGS; do
    run_one "$n" "$alg" "$alg" || fails=$((fails+1))
  done
done

echo
echo "wrote $OUT"
if [[ $fails -eq 0 ]]; then echo "ALL PASS"; exit 0; fi
echo "$fails cell(s) FAILED"; exit 1
