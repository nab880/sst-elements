#!/usr/bin/env bash
# run_all_ecc.sh
#
# End-to-end driver for the VLA-ECC publication pipeline:
#   [optional]  build SST-Core + SST-Elements (Carcosa)
#   step 1.     source mysstconfig.sh, sanity-check binaries
#   step 2.     Phase 1 SST run (real RISC-V binaries) -> phase1.log
#   step 3.     extract_baselines.py --emit-env -> VLA_BASELINE_{CPU,GPU}_PS
#   step 4.     run_ecc_sweep.sh over (BER, scheme, policy, fault_model,
#               due_action, payload_dtype, seed) -> ecc_sweep_out/
#   step 5.     analyze_ecc_results.py -> ecc_sweep_out/analysis/*.csv
#   step 6.     make_figures.py -> ecc_sweep_out/analysis/figs/*.pdf + Table 1
#
# Run from anywhere:
#     ./sst-elements/src/sst/elements/carcosa/VLA-Example/scripts/run_all_ecc.sh
#
# Useful environment variables (all optional):
#   VTO_BUILD_ROOT   repo root containing mysstconfig.sh
#                    (default: nearest ancestor with mysstconfig.sh)
#   OUT_ROOT         output root directory (default: <tests>/ecc_all_out_TIMESTAMP)
#   DO_BUILD=1       run scripts/build-sst-carcosa.sh first
#   SKIP_PHASE1=1    reuse an existing phase1 log (set PHASE1_LOG)
#   SKIP_SWEEP=1     reuse an existing OUT_DIR with logs already present
#   SKIP_ANALYZE=1   skip the analyzer step
#   SKIP_FIGS=1      skip make_figures.py
#   SKIP_BASELINE_AUDIT=1   skip the zero-baseline audit after extraction
#   FAST=1           tiny sweep (1 BER, 1 seed, no jedec_mix) for a smoke run
#   HEADLINE=1       publication-grade sweep on the canonical jedec_mix +
#                    drop_frame motif slice. Locks BERS / SCHEMES / POLICIES /
#                    FAULT_MODELS / DUE_ACTIONS / SEEDS to the values cited in
#                    the paper (see "HEADLINE defaults" below). Use this as
#                    the default publishable path; FAST=1 is for smoke tests
#                    and FULL_CUBE=1 is for the supplement.
#   FULL_CUBE=1      full factorial axes (the legacy default). Used for the
#                    in-repo supplement / artifact, NOT for the main-text
#                    figures and table.
#   PYTHON           python interpreter (default: python3)
#
#   Phase 1 FSM knobs (forwarded to testCarcosaVLA_GPUCPU.py):
#     VLA_NUM_VIT_LAYERS=2, VLA_NUM_LLM_LAYERS=2, VLA_NUM_ACTION_TOKENS=1,
#     VLA_INITIAL_SEQ_LEN=8, VLA_MAX_SEQ_LEN=64
#     VLA_MAX_CYCLES defaults to VLA_PHASE1_MAX_CYCLES, then VLA_PHASE2_MAX_CYCLES,
#     then 1 --- so headline calibration matches Phase 2 actuation depth (8) unless
#     you override. FAST=1 yields 1 actuation for both phases.
#   Any inherited VLA_SST_STOP_AT is cleared for Phase 1 so the binaries can
#   drive the FSM through to the agents' natural Hali exit after max_cycles
#   (wall-clock-unbounded stop-at, same convention as Phase 2).
#
#   Phase 2 matches the same VLA_NUM_* / seq defaults via run_ecc_sweep.sh and
#   runs VLA_MAX_CYCLES actuations per cell, defaulting from VLA_PHASE2_MAX_CYCLES
#   (8 headline / 1 FAST). The sweep also clears VLA_SST_STOP_AT so wall-clock
#   caps cannot truncate the synthetic FSM mid-pipeline.
#
#   Sweep axes (forwarded to run_ecc_sweep.sh):
#     BERS, SCHEMES, POLICIES, SEEDS, FAULT_MODELS, DUE_ACTIONS,
#     PAYLOAD_DTYPE, REGIONS_CSV, KERNEL_POLICY_AWARE, REGION_POLICY_BASE,
#     REGION_POLICY_FULL, FIT_PER_MBIT_PER_HOUR, DRAM_CAPACITY_MB,
#     SIM_TIME_PER_EVENT_NS, CORRECTABLE_PS, DUE_PS, ESCAPE_PS
#   VLA_PHASE1_MAX_CYCLES  optional Phase-1-only max_cycles (default follows
#     VLA_MAX_CYCLES, then VLA_PHASE2_MAX_CYCLES, then 1).
#
#   Figure knobs (forwarded to make_figures.py):
#     UNSAFE_BUDGET    iso-safety budget for Fig. 2 (default: 1e-6)
#
# Exit codes: 0 success, non-zero on first failing step.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXAMPLE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
TESTS_DIR="$(cd "${EXAMPLE_DIR}/tests" && pwd)"
PYTHON="${PYTHON:-python3}"

log()  { echo "[$(date +%H:%M:%S)] $*"; }
warn() { echo "[$(date +%H:%M:%S)] WARN: $*" >&2; }
die()  { echo "[$(date +%H:%M:%S)] ERROR: $*" >&2; exit 1; }

step() {
    local name="$1"; shift
    log "===== $name ====="
    local _t0=$SECONDS
    "$@"
    local _el=$((SECONDS - _t0))
    log "===== $name done (${_el}s) ====="
}

find_repo_root() {
    local d="$SCRIPT_DIR"
    for _ in $(seq 1 12); do
        if [[ -f "$d/mysstconfig.sh" ]]; then
            echo "$d"; return 0
        fi
        d="$(cd "$d/.." && pwd)"
    done
    return 1
}

REPO_ROOT="${VTO_BUILD_ROOT:-}"
if [[ -z "$REPO_ROOT" ]]; then
    REPO_ROOT="$(find_repo_root || true)"
fi
if [[ -z "$REPO_ROOT" || ! -f "$REPO_ROOT/mysstconfig.sh" ]]; then
    warn "mysstconfig.sh not found via VTO_BUILD_ROOT or ancestor walk."
    warn "If 'sst' is already on PATH this is fine; otherwise build/source it first."
fi

if [[ "${DO_BUILD:-0}" == "1" ]]; then
    [[ -n "$REPO_ROOT" ]] || die "DO_BUILD=1 needs VTO_BUILD_ROOT (mysstconfig.sh location)."
    BUILD_SCRIPT="$REPO_ROOT/scripts/build-sst-carcosa.sh"
    [[ -x "$BUILD_SCRIPT" ]] || die "Build script not executable: $BUILD_SCRIPT"
    step "build SST-Core + SST-Elements" bash "$BUILD_SCRIPT"
fi

if [[ -n "$REPO_ROOT" && -f "$REPO_ROOT/mysstconfig.sh" ]]; then
    log "sourcing $REPO_ROOT/mysstconfig.sh"
    # shellcheck source=/dev/null
    source "$REPO_ROOT/mysstconfig.sh"
    # mysstconfig defaults VLA_SST_STOP_AT=2ms for quick dev runs; the ECC
    # pipeline must run each cell to natural FSM exit (run_ecc_sweep also
    # passes env -u VLA_SST_STOP_AT to sst, but clear it here too).
    unset VLA_SST_STOP_AT
fi

command -v sst >/dev/null 2>&1 || die "'sst' not on PATH after sourcing mysstconfig.sh"
command -v "$PYTHON" >/dev/null 2>&1 || die "'$PYTHON' not on PATH"

TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
OUT_ROOT="${OUT_ROOT:-${TESTS_DIR}/ecc_all_out_${TIMESTAMP}}"
mkdir -p "$OUT_ROOT"
log "OUT_ROOT=$OUT_ROOT"

PHASE1_LOG="${PHASE1_LOG:-${OUT_ROOT}/phase1.log}"
SWEEP_DIR="${OUT_DIR:-${OUT_ROOT}/ecc_sweep_out}"
ANALYSIS_DIR="${SWEEP_DIR}/analysis"
mkdir -p "$SWEEP_DIR"

if [[ "${FAST:-0}" == "1" ]]; then
    export BERS="${BERS:-1e-4}"
    export SCHEMES="${SCHEMES:-none secded chipkill}"
    export POLICIES="${POLICIES:-uniform kernel_aware region_aware}"
    export SEEDS="${SEEDS:-1}"
    export FAULT_MODELS="${FAULT_MODELS:-poisson}"
    export DUE_ACTIONS="${DUE_ACTIONS:-latency_only drop_frame}"
    # FAST: keep the cell wall-clock minimal; one frame per cell is enough for
    # the smoke run to verify the pipeline is wired up. The headline path
    # below picks a much larger value.
    export VLA_PHASE2_MAX_CYCLES="${VLA_PHASE2_MAX_CYCLES:-1}"
    export VLA_MAX_CYCLES="$VLA_PHASE2_MAX_CYCLES"
    log "FAST=1 -> tiny sweep: BERS=$BERS SCHEMES=$SCHEMES POLICIES=$POLICIES SEEDS=$SEEDS"
elif [[ "${HEADLINE:-0}" == "1" ]]; then
    # HEADLINE: the publishable axis slice. Locks the sweep to the canonical
    # motif (jedec_mix + drop_frame) on a 4-point BER grid that brackets the
    # unsafe-action budget. With VLA_PHASE2_MAX_CYCLES=8 and 3 seeds the cell
    # pools 24 ACTUATE frames per (scheme, policy, ber) bucket -- enough for
    # narrow Wilson CIs without the full-cube wall-clock. See the paper's
    # methodology section for the design rationale; see run_ecc_sweep.sh for
    # the (scheme=none, policy != uniform) pruning that takes the per-seed
    # cell count from 9 down to 7 (scheme, policy) pairs.
    export BERS="${BERS:-0 1e-7 1e-5 1e-4}"
    export SCHEMES="${SCHEMES:-none secded chipkill}"
    export POLICIES="${POLICIES:-uniform kernel_aware region_aware}"
    export SEEDS="${SEEDS:-1 2 3 4 5 6 7 8}"
    export FAULT_MODELS="${FAULT_MODELS:-jedec_mix}"
    export DUE_ACTIONS="${DUE_ACTIONS:-drop_frame}"
    export VLA_PHASE2_MAX_CYCLES="${VLA_PHASE2_MAX_CYCLES:-8}"
    export VLA_MAX_CYCLES="$VLA_PHASE2_MAX_CYCLES"

    # Compute the expected (scheme, policy) pair count after the
    # run_ecc_sweep.sh pruning (none + non-uniform is skipped) so the log
    # banner shows the actual cell count and a tired-eyes operator can
    # cross-check it against the paper's methodology table.
    n_seeds=$(echo "$SEEDS" | wc -w | tr -d ' ')
    n_bers=$(echo "$BERS" | wc -w | tr -d ' ')
    has_zero=0
    for b in $BERS; do
        if [[ "$b" == "0" || "$b" == "0.0" ]]; then has_zero=1; break; fi
    done
    pairs=0
    for sch in $SCHEMES; do
        for pol in $POLICIES; do
            if [[ "$sch" == "none" && "$pol" != "uniform" ]]; then continue; fi
            pairs=$((pairs + 1))
        done
    done
    n_fm=$(echo "$FAULT_MODELS" | wc -w | tr -d ' ')
    n_due=$(echo "$DUE_ACTIONS" | wc -w | tr -d ' ')
    cells_zero=0
    if [[ "$has_zero" == "1" ]]; then
        cells_zero=$((pairs * n_seeds))
    fi
    n_bers_nonzero=$((n_bers - has_zero))
    cells_nonzero=$((n_bers_nonzero * pairs * n_seeds * n_fm * n_due))
    log "HEADLINE=1 -> motif slice (jedec_mix + drop_frame), VLA_MAX_CYCLES=$VLA_MAX_CYCLES"
    log "  axes: BERS=[$BERS] SCHEMES=[$SCHEMES] POLICIES=[$POLICIES]"
    log "        FAULT_MODELS=[$FAULT_MODELS] DUE_ACTIONS=[$DUE_ACTIONS] SEEDS=[$SEEDS]"
    log "  pairs after pruning (none + non-uniform): $pairs"
    log "  expected cells: BER=0 -> $cells_zero (goldens), BER>0 -> $cells_nonzero, total $((cells_zero + cells_nonzero))"
elif [[ "${FULL_CUBE:-0}" == "1" ]]; then
    # FULL_CUBE: legacy factorial axes used for the artifact / supplement.
    # Whatever the operator does NOT override falls back to run_ecc_sweep.sh
    # defaults (BERS=0..1e-6, both fault models, both due actions, 5 seeds,
    # all four policies). Wall-clock scales ~linearly with the cell count.
    export VLA_PHASE2_MAX_CYCLES="${VLA_PHASE2_MAX_CYCLES:-8}"
    export VLA_MAX_CYCLES="$VLA_PHASE2_MAX_CYCLES"
    log "FULL_CUBE=1 -> legacy factorial sweep, VLA_MAX_CYCLES=$VLA_MAX_CYCLES (~1k+ Phase-2 cells)"
else
    # Default path: same Wilson-CI floor as HEADLINE but without locking the
    # axes -- useful when the operator is iterating on a single knob. The
    # publishable runs should set HEADLINE=1; this branch deliberately does
    # NOT call itself "headline sweep" anymore so logs aren't misleading.
    export VLA_PHASE2_MAX_CYCLES="${VLA_PHASE2_MAX_CYCLES:-8}"
    export VLA_MAX_CYCLES="$VLA_PHASE2_MAX_CYCLES"
    log "default sweep (no HEADLINE=1 / FULL_CUBE=1 set): VLA_MAX_CYCLES=$VLA_MAX_CYCLES"
    log "  honoring whatever BERS / SCHEMES / POLICIES / SEEDS / FAULT_MODELS / DUE_ACTIONS"
    log "  the operator (or run_ecc_sweep.sh) provides. Set HEADLINE=1 for the publishable slice."
fi

run_phase1() {
    if [[ "${SKIP_PHASE1:-0}" == "1" ]]; then
        [[ -f "$PHASE1_LOG" ]] || die "SKIP_PHASE1=1 but PHASE1_LOG=$PHASE1_LOG missing."
        log "SKIP_PHASE1=1, reusing $PHASE1_LOG"
        return
    fi
    if [[ ! -x "$TESTS_DIR/vla_cpu" || ! -x "$TESTS_DIR/vla_gpu" ]]; then
        warn "vla_cpu / vla_gpu RISC-V binaries not present in $TESTS_DIR."
        warn "Skipping Phase 1; baselines will fall back to safe defaults."
        : > "$PHASE1_LOG"
        return
    fi
    log "Phase 1 -> $PHASE1_LOG"
    # Phase 1 must run to completion: no wall-clock stop-at, and enough
    # max_cycles to match Phase 2 so baselines and golden trajectories see the
    # same actuation count. An inherited VLA_SST_STOP_AT can truncate mid-FSM.
    (cd "$TESTS_DIR" && env -u VLA_SST_STOP_AT \
        VLA_NUM_VIT_LAYERS="${VLA_NUM_VIT_LAYERS:-2}" \
        VLA_NUM_LLM_LAYERS="${VLA_NUM_LLM_LAYERS:-2}" \
        VLA_MAX_CYCLES="${VLA_MAX_CYCLES:-${VLA_PHASE1_MAX_CYCLES:-${VLA_PHASE2_MAX_CYCLES:-1}}}" \
        VLA_INITIAL_SEQ_LEN="${VLA_INITIAL_SEQ_LEN:-8}" \
        VLA_MAX_SEQ_LEN="${VLA_MAX_SEQ_LEN:-64}" \
        VLA_NUM_ACTION_TOKENS="${VLA_NUM_ACTION_TOKENS:-1}" \
        sst testCarcosaVLA_GPUCPU.py 2>&1 | tee "$PHASE1_LOG")
}

extract_baselines() {
    local extract="$SCRIPT_DIR/extract_baselines.py"
    [[ -f "$extract" ]] || die "extract_baselines.py not found at $extract"
    if [[ -s "$PHASE1_LOG" ]]; then
        log "extract_baselines.py --emit-env $PHASE1_LOG"
        local emitted
        emitted="$("$PYTHON" "$extract" --emit-env "$PHASE1_LOG")" || true
        if [[ -n "$emitted" ]]; then
            log "  $emitted"
            eval "$emitted"
        else
            warn "extract_baselines.py emitted nothing; using fallback baselines."
        fi
    else
        warn "Phase 1 log empty; using fallback baselines."
    fi
    export VLA_BASELINE_CPU_PS="${VLA_BASELINE_CPU_PS:-${VLA_BASELINE_CPU_CYCLES:-200000}}"
    export VLA_BASELINE_GPU_PS="${VLA_BASELINE_GPU_PS:-${VLA_BASELINE_GPU_CYCLES:-150000}}"
    log "VLA_BASELINE_CPU_PS=$VLA_BASELINE_CPU_PS  VLA_BASELINE_GPU_PS=$VLA_BASELINE_GPU_PS"
}

audit_baselines() {
    if [[ "${SKIP_BASELINE_AUDIT:-0}" == "1" ]]; then
        log "SKIP_BASELINE_AUDIT=1, skipping zero-baseline audit."
        return
    fi
    if [[ ! -s "$PHASE1_LOG" ]]; then
        warn "Phase 1 log empty; baselines came from fallback constants -> skipping audit."
        warn "Re-run Phase 1 with working vla_cpu/vla_gpu binaries to get real baselines."
        return
    fi
    local extract="$SCRIPT_DIR/extract_baselines.py"
    [[ -f "$extract" ]] || die "extract_baselines.py not found at $extract"
    log "auditing per-kernel baselines in $PHASE1_LOG"
    if ! "$PYTHON" "$extract" --audit "$PHASE1_LOG"; then
        die "Baseline audit failed: one or more kernel slots are zero (see above). \
Re-run Phase 1 unbounded (unset VLA_SST_STOP_AT) and check VLA_MAX_CYCLES / \
VLA_NUM_*_LAYERS / VLA_NUM_ACTION_TOKENS, or set SKIP_BASELINE_AUDIT=1 to override."
    fi
}

run_sweep() {
    if [[ "${SKIP_SWEEP:-0}" == "1" ]]; then
        [[ -f "$SWEEP_DIR/index.csv" ]] || die "SKIP_SWEEP=1 but $SWEEP_DIR/index.csv missing."
        log "SKIP_SWEEP=1, reusing $SWEEP_DIR"
        return
    fi
    local sweep="$SCRIPT_DIR/run_ecc_sweep.sh"
    [[ -x "$sweep" ]] || die "run_ecc_sweep.sh not executable at $sweep"
    log "ECC sweep -> $SWEEP_DIR"
    (cd "$TESTS_DIR" && OUT_DIR="$SWEEP_DIR" bash "$sweep")
}

run_analyze() {
    if [[ "${SKIP_ANALYZE:-0}" == "1" ]]; then
        log "SKIP_ANALYZE=1, skipping analyzer."
        return
    fi
    local analyze="$SCRIPT_DIR/analyze_ecc_results.py"
    [[ -f "$analyze" ]] || die "analyze_ecc_results.py not found at $analyze"
    log "analyzer -> $ANALYSIS_DIR"
    local args=("$SWEEP_DIR" --out "$ANALYSIS_DIR")
    # HEADLINE locks in jedec_mix + drop_frame; the canonical slice keeps
    # pressure_points.csv from being duplicated across the supplement axes.
    # The full factorial is preserved as pressure_points_full.csv.
    if [[ "${HEADLINE:-0}" == "1" ]]; then
        args+=("--canonical-slice" "${CANONICAL_SLICE:-jedec_mix+drop_frame}")
    elif [[ -n "${CANONICAL_SLICE:-}" ]]; then
        args+=("--canonical-slice" "$CANONICAL_SLICE")
    fi
    if [[ -n "${UNSAFE_BUDGET:-}" ]]; then
        args+=("--unsafe-budget" "$UNSAFE_BUDGET")
    fi
    if [[ "${POLICY_CONTRASTS:-1}" == "1" ]]; then
        args+=("--policy-contrasts")
    fi
    "$PYTHON" "$analyze" "${args[@]}"
}

run_figures() {
    if [[ "${SKIP_FIGS:-0}" == "1" ]]; then
        log "SKIP_FIGS=1, skipping make_figures.py."
        return
    fi
    local mkfigs="$SCRIPT_DIR/make_figures.py"
    [[ -f "$mkfigs" ]] || die "make_figures.py not found at $mkfigs"
    if ! "$PYTHON" -c "import matplotlib, pandas" >/dev/null 2>&1; then
        warn "matplotlib + pandas not importable; figures step skipped."
        warn "Install with: $PYTHON -m pip install matplotlib pandas"
        return
    fi
    log "make_figures.py -> $ANALYSIS_DIR/figs"
    local args=("$ANALYSIS_DIR")
    if [[ -n "${UNSAFE_BUDGET:-}" ]]; then
        args+=("--unsafe-budget" "$UNSAFE_BUDGET")
    fi
    "$PYTHON" "$mkfigs" "${args[@]}"
}

PIPELINE_T0=$SECONDS

step "Phase 1 (real binaries)" run_phase1
step "Baseline extraction"     extract_baselines
step "Baseline audit"          audit_baselines
step "Phase 2 ECC sweep"       run_sweep
step "Analysis"                run_analyze
step "Figures + Table 1"       run_figures

TOTAL=$((SECONDS - PIPELINE_T0))
log ""
log "All done in ${TOTAL}s."
log "Outputs:"
log "  Phase 1 log : $PHASE1_LOG"
log "  Sweep logs  : $SWEEP_DIR (index: $SWEEP_DIR/index.csv)"
log "  Analysis    : $ANALYSIS_DIR"
log "  Figures     : $ANALYSIS_DIR/figs/"
log "  Table 1     : $ANALYSIS_DIR/table1_headline.csv"
