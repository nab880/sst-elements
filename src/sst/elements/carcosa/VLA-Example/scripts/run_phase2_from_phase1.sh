#!/usr/bin/env bash
# Phase 2 from Phase1 log: extract_baselines.py --emit-env then sst testCarcosaVLA_GPUCPU_Synth.py.
# Usage: [--run-phase1] [phase1.log]. Env: VTO_BUILD_ROOT, VLA_SCALE_FACTOR, PHASE2_LOG, PYTHON.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TESTS_DIR="$(cd "${SCRIPT_DIR}/../tests" && pwd)"

usage() {
  cat <<'EOF'
Usage: run_phase2_from_phase1.sh [--run-phase1] [phase1.log]

  --run-phase1   Run Phase 1 (testCarcosaVLA_GPUCPU.py), tee to phase1.log, then Phase 2.
  phase1.log     Path to existing Phase 1 output (default: ../tests/phase1_last.log).

SST is run from VLA-Example/tests so Python configs and RISC-V binaries resolve.

Environment: VTO_BUILD_ROOT, VLA_SCALE_FACTOR, PHASE2_LOG, PYTHON
EOF
}

find_repo_root() {
  local d="$SCRIPT_DIR"
  local i
  for i in $(seq 1 12); do
    if [[ -f "$d/mysstconfig.sh" ]]; then
      echo "$d"
      return 0
    fi
    d="$(cd "$d/.." && pwd)"
  done
  return 1
}

REPO_ROOT="${VTO_BUILD_ROOT:-}"
if [[ -z "$REPO_ROOT" ]]; then
  REPO_ROOT="$(find_repo_root)" || true
fi
if [[ -z "$REPO_ROOT" || ! -f "$REPO_ROOT/mysstconfig.sh" ]]; then
  echo "ERROR: Could not find mysstconfig.sh. Set VTO_BUILD_ROOT to your vto-build (or vto) tree root." >&2
  exit 1
fi
# shellcheck source=/dev/null
source "$REPO_ROOT/mysstconfig.sh"

PHASE1_LOG="${TESTS_DIR}/phase1_last.log"
RUN_PHASE1=0
PYTHON="${PYTHON:-python3}"
EXTRACT="${SCRIPT_DIR}/extract_baselines.py"

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --run-phase1)
      RUN_PHASE1=1
      shift
      ;;
    *)
      PHASE1_LOG="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
      shift
      ;;
  esac
done

wall_clock_fmt() {
  local t=$1
  if [[ -z "$t" || "$t" -eq 0 ]]; then
    echo "0s"
    return
  fi
  local h=$((t / 3600)) m=$(((t % 3600) / 60)) s=$((t % 60))
  if (( h > 0 )); then
    echo "${h}h ${m}m ${s}s"
  elif (( m > 0 )); then
    echo "${m}m ${s}s"
  else
    echo "${s}s"
  fi
}

PIPELINE_T0=$SECONDS

run_phase1() {
  echo "=== Phase 1: testCarcosaVLA_GPUCPU.py -> ${PHASE1_LOG} ===" >&2
  (cd "${TESTS_DIR}" && sst testCarcosaVLA_GPUCPU.py 2>&1 | tee "${PHASE1_LOG}")
}

run_phase2() {
  echo "=== Phase 2: testCarcosaVLA_GPUCPU_Synth.py (VLA_SCALE_FACTOR=${VLA_SCALE_FACTOR}) ===" >&2
  if [[ -n "${PHASE2_LOG:-}" ]]; then
    (cd "${TESTS_DIR}" && sst testCarcosaVLA_GPUCPU_Synth.py 2>&1 | tee "${PHASE2_LOG}")
  else
    (cd "${TESTS_DIR}" && sst testCarcosaVLA_GPUCPU_Synth.py)
  fi
}

if [[ "$RUN_PHASE1" -eq 1 ]]; then
  _t0=$SECONDS
  run_phase1
  _elapsed=$((SECONDS - _t0))
  echo "=== Wall clock: Phase 1 (full model SST) ${_elapsed}s ($(wall_clock_fmt "${_elapsed}")) ===" >&2
fi

if [[ ! -f "${PHASE1_LOG}" ]]; then
  echo "ERROR: Phase 1 log not found: ${PHASE1_LOG}" >&2
  echo "  Use --run-phase1 to generate it, or pass an existing log path." >&2
  exit 1
fi

echo "=== Extracting baselines (extract_baselines.py --emit-env) ===" >&2
_t0=$SECONDS
eval "$("${PYTHON}" "${EXTRACT}" --emit-env "${PHASE1_LOG}")"
_elapsed=$((SECONDS - _t0))
echo "=== Wall clock: baseline extraction ${_elapsed}s ($(wall_clock_fmt "${_elapsed}")) ===" >&2

if [[ -z "${VLA_BASELINE_CPU_PS:-}" && -n "${VLA_BASELINE_CPU_CYCLES:-}" ]]; then
  echo "=== [deprecated] Promoting VLA_BASELINE_CPU_CYCLES -> VLA_BASELINE_CPU_PS ===" >&2
  export VLA_BASELINE_CPU_PS="${VLA_BASELINE_CPU_CYCLES}"
fi
if [[ -z "${VLA_BASELINE_GPU_PS:-}" && -n "${VLA_BASELINE_GPU_CYCLES:-}" ]]; then
  echo "=== [deprecated] Promoting VLA_BASELINE_GPU_CYCLES -> VLA_BASELINE_GPU_PS ===" >&2
  export VLA_BASELINE_GPU_PS="${VLA_BASELINE_GPU_CYCLES}"
fi

export VLA_SCALE_FACTOR="${VLA_SCALE_FACTOR:-1.0}"

_t0=$SECONDS
run_phase2
_elapsed=$((SECONDS - _t0))
echo "=== Wall clock: Phase 2 (synth SST) ${_elapsed}s ($(wall_clock_fmt "${_elapsed}")) ===" >&2

_total=$((SECONDS - PIPELINE_T0))
echo "=== Wall clock: pipeline total ${_total}s ($(wall_clock_fmt "${_total}")) ===" >&2
