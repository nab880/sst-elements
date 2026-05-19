#!/usr/bin/env bash
# Phase-2 ECC pressure-point sweep over (BER, scheme, policy_mode, seed,
# fault_model, due_action, region_policy).
#
# Run from VLA-Example/tests/. Requires VLA_BASELINE_CPU_PS / VLA_BASELINE_GPU_PS
# (from extract_baselines.py) and a sourced mysstconfig.sh.
#
# Optional knobs (any that are unset use the default):
#   BERS                 BER list to sweep                            (1e-12 ... 1e-6)
#   SCHEMES              "none secded chipkill"
#   POLICIES             "uniform kernel_aware region_aware full"
#   SEEDS                "1 2 3 4 5"
#   FAULT_MODELS         "poisson jedec_mix"            (Phase 2)
#   DUE_ACTIONS          "latency_only drop_frame"      (Phase 3)
#   REGIONS_CSV          published-region map for the agent
#   KERNEL_POLICY_AWARE  CSV used when policy=kernel_aware
#   REGION_POLICY_BASE   CSV used when policy=region_aware (template, %BER% substituted)
#   REGION_POLICY_FULL   CSV used when policy=full (kernel x region overrides)
#   PAYLOAD_DTYPE        bytes|bf16|fp8|int8 (default: bytes)
#   FIT_PER_MBIT_PER_HOUR / DRAM_CAPACITY_MB / SIM_TIME_PER_EVENT_NS
#                        FIT-mode calibration (only consumed by JedecMix)
#   VLA_DECODE_EXIT_PROB FSM decode-loop early-exit probability (default 0.0).
#                        Set to e.g. 0.25 to make the FSM kernel trace itself
#                        vary across seeds; at 0.0 the FSM is deterministic
#                        regardless of seed (only fault draws vary).
#
#   --- New in audit pass (all opt-in, default behavior unchanged) ---
#   RESUME=1             keep existing $OUT_DIR/index.csv, skip cells whose
#                        log is already present with rc=0. Useful after a
#                        Ctrl-C; without it the index is truncated every run.
#   JOBS=N               run BER>0 cells N at a time via background processes
#                        (BER=0 stays serial because goldens must exist before
#                        any BER>0 cell that consumes them). Defaults to 1.
#   LOG_FILTER=1         strip init-phase noise (Phase: Init/Construction/
#                        forwarding/received HaliEvent/highlink_=) before
#                        writing each log; cuts log size ~5x without losing
#                        any structured block consumed by analyze_ecc_results.py.
#
#   VLA_MAX_CYCLES       FSM pipeline cycles (actuations) per Phase-2 cell
#                        (forwarded to testCarcosaVLA_GPUCPU_Synth.py). When
#                        unset, defaults to VLA_PHASE2_MAX_CYCLES if set, else 8.
#                        run_all_ecc.sh sets VLA_PHASE2_MAX_CYCLES (8 headline /
#                        1 FAST). The sweep must pass this through --- the Python
#                        script alone defaults max_cycles=1, which ends the
#                        delay-agent FSM after a single actuation.
#   VLA_SST_STOP_AT      SST simulation wall-clock cap. The sweep always runs
#                        with this unset so the run ends on the delay agent's
#                        natural Hali exit after max_cycles (Phase 1 already
#                        clears stop-at for real binaries; Phase 2 must too).
#
# Seed propagation:
#   The sweep's `seed` knob is threaded into BOTH the EccGuard's fault RNG
#   (via ECC_SEED) AND the CPU delay agent's FSM RNG (via VLA_RNG_SEED).
#   With VLA_DECODE_EXIT_PROB=0.0 (default) the FSM RNG is never consumed, so
#   the trace stays fixed; bump the exit prob to exercise FSM diversity.
#
#   CAVEAT when VLA_DECODE_EXIT_PROB > 0: the golden trajectory captured by
#   the BER=0 run becomes seed-dependent (different seeds see different
#   decode-loop lengths). The current golden mechanism keys on (scheme,
#   policy) only, so all seeds get compared to seed=1's golden. If you
#   enable decode_exit_prob, either (a) pin SEEDS to a single value, or
#   (b) extend golden_path_for / golden_target to include the seed.
#
# Golden semantics (see README_ECC_METHODOLOGY.md section 4.1 for detail):
#   - Each (scheme, policy) pair has exactly one golden file at
#     $GOLDEN_DIR/golden_${scheme}_${policy}.csv. The first successful BER=0
#     run for that pair writes it; later BER=0 cells with other seeds reuse
#     it (use ACTION_SCORER_FORCE_REGEN_GOLDEN=1 to overwrite).
#   - BER>0 cells pass the file as ACTION_SCORER_GOLDEN. The Action Scorer
#     fatals at setup() if the file is missing or empty, instead of silently
#     reporting unsafe_action_rate=0 for every frame. Set
#     ACTION_SCORER_GOLDEN_REQUIRED=0 to opt out of the fail-loud check.

set -u

OUT_DIR="${OUT_DIR:-./ecc_sweep_out}"
mkdir -p "$OUT_DIR"

# BERS list starts with "0" so every (scheme, policy, fault_model, due_action,
# seed) group has a fault-free reference cell in per_run_summary.csv; the BER=0
# run also doubles as the ActionScorer golden source (see run_one).
#
# At BER=0 fault_model / due_action are inert (no draws fire), so the BER=0
# sim is now run *exactly once* per (scheme, policy, seed) using the first
# (fm, due) tuple as canonical labels. The earlier sweep re-emitted four
# identical index rows per BER=0 cell, which inflated downstream per-row
# tables 4x; the dedup fixes that without changing the actual simulations.
BERS="${BERS:-0 1e-12 1e-11 1e-10 1e-9 1e-8 1e-7 1e-6}"
SCHEMES="${SCHEMES:-none secded chipkill}"
POLICIES="${POLICIES:-uniform kernel_aware region_aware full}"
SEEDS="${SEEDS:-1 2 3 4 5}"
FAULT_MODELS="${FAULT_MODELS:-poisson jedec_mix}"
DUE_ACTIONS="${DUE_ACTIONS:-latency_only drop_frame}"
PAYLOAD_DTYPE="${PAYLOAD_DTYPE:-bytes}"

CORRECTABLE_PS="${CORRECTABLE_PS:-5000}"
DUE_PS="${DUE_PS:-20000}"
ESCAPE_PS="${ESCAPE_PS:-0}"
SST_CFG="${SST_CFG:-testCarcosaVLA_GPUCPU_Synth.py}"

RESUME="${RESUME:-0}"
JOBS="${JOBS:-1}"
LOG_FILTER="${LOG_FILTER:-0}"

# Phase-2 FSM depth: prefer explicit VLA_MAX_CYCLES, else VLA_PHASE2_MAX_CYCLES
# (set by run_all_ecc.sh), else a publication-friendly multi-actuation default.
VLA_MAX_CYCLES="${VLA_MAX_CYCLES:-${VLA_PHASE2_MAX_CYCLES:-8}}"

# Default workload region map. Synthetic addresses; the agent publishes them
# into PipelineStateBase::regions slot 1..N so EccGuard can route.
REGIONS_CSV="${REGIONS_CSV:-\
weights:0x10000000:0x4000000,\
kv_cache:0x14000000:0x100000,\
activations:0x14100000:0x200000,\
action_queue:0x14300000:0x1000}"

# Kernel-aware policy: harden loop-carried memory-bound kernels; bypass LM_HEAD argmax.
KERNEL_POLICY_AWARE="${KERNEL_POLICY_AWARE:-\
KV_CACHE_ATTN:chipkill:%BER%:8000:30000:0,\
DECODE_FFN:secded:%BER%:5000:20000:0,\
GEMV_PROJECT:secded:%BER%:5000:20000:0,\
LM_HEAD:none:0:0:0:0}"

# Region-aware policy: protect weights / kv_cache; relax activations.
REGION_POLICY_BASE="${REGION_POLICY_BASE:-\
*@weights:chipkill:%BER%:8000:30000:0,\
*@kv_cache:secded:%BER%:5000:20000:0,\
*@activations:none:0:0:0:0,\
*@action_queue:chipkill:%BER%:8000:30000:0}"

# Full kernel x region: kernel-aware overrides AND region-aware overrides.
REGION_POLICY_FULL="${REGION_POLICY_FULL:-\
KV_CACHE_ATTN@kv_cache:chipkill:%BER%:8000:30000:0,\
LM_HEAD@weights:secded:%BER%:5000:20000:0,\
*@weights:chipkill:%BER%:8000:30000:0,\
*@activations:none:0:0:0:0}"

if [ -z "${VLA_BASELINE_CPU_PS:-}" ] || [ -z "${VLA_BASELINE_GPU_PS:-}" ]; then
    echo "ERROR: VLA_BASELINE_CPU_PS / VLA_BASELINE_GPU_PS must be set." >&2
    echo "Hint: eval \"\$(python3 ../scripts/extract_baselines.py --emit-env phase1.log)\"" >&2
    exit 1
fi

if ! command -v sst >/dev/null 2>&1; then
    echo "ERROR: 'sst' not on PATH; did you source mysstconfig.sh?" >&2
    exit 1
fi

INDEX_CSV="$OUT_DIR/index.csv"
GOLDEN_DIR="$OUT_DIR/goldens"
mkdir -p "$GOLDEN_DIR"

# Per-process index fragments live here while JOBS>1 so background workers
# don't race on a shared INDEX_CSV. The parent concatenates them at the end.
INDEX_PART_DIR="$OUT_DIR/.index_parts"

if [ "$RESUME" = "1" ] && [ -s "$INDEX_CSV" ]; then
    echo "RESUME=1: keeping existing $INDEX_CSV (use unset/empty to start fresh)"
else
    echo "ber,scheme,policy,seed,fault_model,due_action,log,exit,golden,emitted_golden" > "$INDEX_CSV"
fi
rm -rf "$INDEX_PART_DIR"
mkdir -p "$INDEX_PART_DIR"

# is_ber_numeric: strict numeric check. Accepts integers, decimals, and
# scientific notation; rejects everything else. Used to gate is_ber_zero so a
# typo'd entry like BERS="0 1e-9 abc" is caught instead of silently routed
# through the BER=0 reuse path (where it would re-emit goldens).
is_ber_numeric() {
    [[ "$1" =~ ^[+-]?([0-9]+(\.[0-9]*)?|\.[0-9]+)([eE][+-]?[0-9]+)?$ ]]
}

# is_ber_zero handles 0, 0.0, 0e0, 0.0e0 -> true; non-numeric strings now fail
# fast instead of being treated as zero.
is_ber_zero() {
    is_ber_numeric "$1" || return 1
    awk -v b="$1" 'BEGIN { exit !(b+0 == 0) }'
}

# Preflight: validate every BER value before launching any sst process.
for _b in $BERS; do
    if ! is_ber_numeric "$_b"; then
        echo "ERROR: BER value '$_b' is not numeric. Check BERS='$BERS'." >&2
        exit 1
    fi
done

policy_csv_for() {
    local policy="$1"
    local ber="$2"
    case "$policy" in
        uniform)      printf '' ;;
        kernel_aware) printf '%s' "${KERNEL_POLICY_AWARE//%BER%/$ber}" ;;
        region_aware) printf '%s' "${REGION_POLICY_BASE//%BER%/$ber}" ;;
        full)         printf '%s' "${REGION_POLICY_FULL//%BER%/$ber}" ;;
        *)            echo "WARN: unknown policy '$policy', falling back to uniform" >&2
                      printf '' ;;
    esac
}

# golden_path_for prints the golden CSV path on stdout iff a non-empty one
# exists for this (scheme, policy); otherwise it prints nothing.
golden_path_for() {
    local p="$GOLDEN_DIR/golden_${1}_${2}.csv"
    [ -s "$p" ] && printf '%s' "$p"
}

# already_indexed: returns 0 (true) when RESUME=1 AND the (ber, scheme, policy,
# seed, fm, due, logname) tuple is already present in $INDEX_CSV with exit=0.
# Used to skip cells that completed in a prior interrupted run.
already_indexed() {
    [ "$RESUME" != "1" ] && return 1
    [ -s "$INDEX_CSV" ] || return 1
    local ber="$1" scheme="$2" policy="$3" seed="$4" fm="$5" due="$6" log="$7"
    grep -q -F "${ber},${scheme},${policy},${seed},${fm},${due},${log},0," "$INDEX_CSV"
}

# append_index: writes one row to a per-process fragment to avoid contention
# when JOBS>1. The parent concatenates fragments into $INDEX_CSV at the end.
append_index() {
    local frag="$INDEX_PART_DIR/idx_$$_${RANDOM}.csv"
    printf '%s\n' "$1" >> "$frag"
}

# run_sst: invokes sst with the given environment, optionally piping through
# a noise filter (LOG_FILTER=1). Returns the sst exit code, not the filter's.
run_sst() {
    local logpath="$1"
    # Never inherit a wall-clock stop-at: it truncates the FSM mid-pipeline
    # while the Python config defaults to stop-at "0 ns" (run to exit event).
    if [ "$LOG_FILTER" = "1" ]; then
        # PIPESTATUS preserves sst's rc even when grep returns 1 (no matches).
        env -u VLA_SST_STOP_AT sst "$SST_CFG" 2>&1 \
            | grep -Ev '^(Phase: (Init|Construction|Complete|Setup|Run|Finish|Destruction)|.*forwarding event from|.*received HaliEvent|.*highlink_=|Emergency shutdown:)' \
            > "$logpath"
        return "${PIPESTATUS[0]}"
    else
        env -u VLA_SST_STOP_AT sst "$SST_CFG" > "$logpath" 2>&1
        return $?
    fi
}

run_one() {
    local ber="$1"
    local scheme="$2"
    local policy="$3"
    local seed="$4"
    local fmodel="$5"
    local due="$6"
    local golden_csv="${7:-}"

    local kernel_policy
    kernel_policy="$(policy_csv_for "$policy" "$ber")"

    local zero=0
    is_ber_zero "$ber" && zero=1

    local logname
    if [ "$zero" = "1" ]; then
        # BER=0 dedup: one log per (scheme, policy, seed) regardless of
        # (fm, due). Even though the BER=0 inner loop only runs once now,
        # we keep the bare logname so RESUME picks up cleanly.
        logname="run_${scheme}_${policy}_ber0_seed${seed}.log"
    else
        logname="run_${scheme}_${policy}_${fmodel}_${due}_ber${ber}_seed${seed}.log"
    fi
    local logpath="$OUT_DIR/$logname"
    local rcpath="${logpath}.rc"

    if already_indexed "$ber" "$scheme" "$policy" "$seed" "$fmodel" "$due" "$logname"; then
        echo "=== ber=$ber scheme=$scheme policy=$policy fm=$fmodel due=$due seed=$seed (resume skip) ==="
        return 0
    fi

    local rc=0
    local t0=$SECONDS
    if [ "$zero" = "1" ] && [ -s "$logpath" ]; then
        # Reuse path: replay the first run's rc from the sidecar so a failed
        # BER=0 invocation doesn't masquerade as success here. Pre-audit
        # logs without a sidecar are conservatively treated as rc=0.
        if [ -s "$rcpath" ]; then
            rc=$(cat "$rcpath" 2>/dev/null || echo 0)
        fi
        echo "=== ber=$ber scheme=$scheme policy=$policy fm=$fmodel due=$due seed=$seed (reuse $logname rc=$rc) ==="
    else
        echo "=== ber=$ber scheme=$scheme policy=$policy fm=$fmodel due=$due seed=$seed -> $logname ==="

        local emit_golden=0
        local golden_arg="$golden_csv"
        if [ "$zero" = "1" ]; then
            # BER=0 IS the golden; don't compare it to a (possibly stale) file
            # and tell the scorer to emit its trajectory so we can capture it.
            emit_golden=1
            golden_arg=""
        fi

        ECC_SCHEME="$scheme" \
        ECC_BER="$ber" \
        ECC_CORRECTABLE_LATENCY_PS="$CORRECTABLE_PS" \
        ECC_DUE_LATENCY_PS="$DUE_PS" \
        ECC_ESCAPE_LATENCY_PS="$ESCAPE_PS" \
        ECC_KERNEL_POLICY="$kernel_policy" \
        ECC_FAULT_MODEL="$fmodel" \
        ECC_DUE_ACTION="$due" \
        ECC_PAYLOAD_DTYPE="$PAYLOAD_DTYPE" \
        ECC_FIT_PER_MBIT_PER_HOUR="${FIT_PER_MBIT_PER_HOUR:-0}" \
        ECC_DRAM_CAPACITY_MB="${DRAM_CAPACITY_MB:-1024}" \
        ECC_SIM_TIME_PER_EVENT_NS="${SIM_TIME_PER_EVENT_NS:-100}" \
        ECC_SEED="$seed" \
        VLA_RNG_SEED="$seed" \
        VLA_DECODE_EXIT_PROB="${VLA_DECODE_EXIT_PROB:-0.0}" \
        VLA_MAX_CYCLES="$VLA_MAX_CYCLES" \
        VLA_NUM_VIT_LAYERS="${VLA_NUM_VIT_LAYERS:-2}" \
        VLA_NUM_LLM_LAYERS="${VLA_NUM_LLM_LAYERS:-2}" \
        VLA_INITIAL_SEQ_LEN="${VLA_INITIAL_SEQ_LEN:-8}" \
        VLA_MAX_SEQ_LEN="${VLA_MAX_SEQ_LEN:-64}" \
        VLA_NUM_ACTION_TOKENS="${VLA_NUM_ACTION_TOKENS:-1}" \
        VLA_REGIONS="$REGIONS_CSV" \
        ACTION_SCORER_GOLDEN="$golden_arg" \
        ACTION_SCORER_EMIT_GOLDEN="$emit_golden" \
        run_sst "$logpath"
        rc=$?
        echo "$rc" > "$rcpath"

        # First BER=0 run captures the golden CSV consumed by BER>0 cells.
        if [ "$zero" = "1" ] && [ $rc -eq 0 ]; then
            local golden_target="$GOLDEN_DIR/golden_${scheme}_${policy}.csv"
            if [ ! -s "$golden_target" ] || [ "${ACTION_SCORER_FORCE_REGEN_GOLDEN:-0}" = "1" ]; then
                awk '
                    /^=== Action Scorer .* Golden Emit ===$/      { capture=1; next }
                    /^=== End Action Scorer .* Golden Emit ===$/  { capture=0; next }
                    capture { print }
                ' "$logpath" > "$golden_target"
                if [ -s "$golden_target" ]; then
                    local frames
                    frames=$(awk 'NR>1' "$golden_target" | wc -l | tr -d ' ')
                    echo "  [golden] -> $golden_target ($frames frames)"
                else
                    echo "  [golden] WARN: empty emit block; comparison disabled for ($scheme,$policy)"
                    rm -f "$golden_target"
                fi
            fi
        fi
    fi

    local elapsed=$((SECONDS - t0))

    # Record both the golden that WOULD be applied to this row (golden_for_idx)
    # and the golden that this row EMITTED, if any. The latter makes BER=0
    # rows self-describing instead of leaving the operator to cross-reference
    # the goldens/ directory.
    local golden_for_idx=""
    if [ "$zero" != "1" ] && [ -n "$golden_csv" ]; then
        golden_for_idx="${golden_csv##*/}"
    fi
    local emitted_golden_for_idx=""
    if [ "$zero" = "1" ] && [ -s "$GOLDEN_DIR/golden_${scheme}_${policy}.csv" ]; then
        emitted_golden_for_idx="golden_${scheme}_${policy}.csv"
    fi

    append_index "$ber,$scheme,$policy,$seed,$fmodel,$due,$logname,$rc,$golden_for_idx,$emitted_golden_for_idx"
    if [ $rc -ne 0 ]; then
        echo "  -> exit $rc elapsed=${elapsed}s (see $logpath)"
    else
        echo "  -> rc=0 elapsed=${elapsed}s"
    fi
}

# wait_for_slot: blocks until the live background-job count drops below $JOBS.
# Tracks pids in a global array; called by Phase 2 before each spawn.
wait_for_slot() {
    local max="$1"
    while [ ${#BG_PIDS[@]} -ge "$max" ]; do
        # Scan all tracked pids and reap any that have finished, avoiding
        # head-of-line blocking when the oldest job is slow.
        local new_pids=()
        local reaped=0
        for pid in "${BG_PIDS[@]}"; do
            if kill -0 "$pid" 2>/dev/null; then
                new_pids+=("$pid")
            else
                wait "$pid" 2>/dev/null || true
                reaped=1
            fi
        done
        BG_PIDS=("${new_pids[@]}")
        if [ "$reaped" -eq 0 ] && [ ${#BG_PIDS[@]} -ge "$max" ]; then
            # All pids still running; sleep briefly then retry.
            sleep 0.5
        fi
    done
}

BG_PIDS=()

# Reset SECONDS so the wallclock summary at the end reflects only this run.
PHASE1_T0=$SECONDS

# Helper that picks the canonical (fm, due) used to label BER=0 rows.
first_word() { echo "$1" | awk '{print $1}'; }
CANON_FM="$(first_word "$FAULT_MODELS")"
CANON_DUE="$(first_word "$DUE_ACTIONS")"

# Phase 1: BER=0 cells (serial). Each (scheme, policy, seed) runs sst exactly
# once and writes a single index row labeled with the canonical (fm, due).
# Goldens are produced as a side effect (see run_one).
echo ""
echo "=== Phase 1: BER=0 baselines (goldens) ==="
for ber in $BERS; do
    is_ber_zero "$ber" || continue
    for scheme in $SCHEMES; do
        for policy in $POLICIES; do
            if [ "$scheme" = "none" ] && [ "$policy" != "uniform" ]; then
                continue
            fi
            for seed in $SEEDS; do
                run_one "$ber" "$scheme" "$policy" "$seed" "$CANON_FM" "$CANON_DUE" ""
            done
        done
    done
done
PHASE1_EL=$((SECONDS - PHASE1_T0))

# Golden audit: BER>0 cells consume goldens generated above. If any are
# missing the ActionScorer silently degrades to "first observed checksum is
# golden", which makes downstream argmax_change_rate / unsafe_action_rate
# meaningless. Surface it loudly here before launching the expensive phase 2.
HAS_NONZERO=0
for _b in $BERS; do is_ber_zero "$_b" || HAS_NONZERO=1; done
MISSING_GOLDENS=0
if [ "$HAS_NONZERO" = "1" ]; then
    for scheme in $SCHEMES; do
        for policy in $POLICIES; do
            if [ "$scheme" = "none" ] && [ "$policy" != "uniform" ]; then
                continue
            fi
            target="$GOLDEN_DIR/golden_${scheme}_${policy}.csv"
            if [ ! -s "$target" ]; then
                echo "WARN: missing golden for ($scheme, $policy) -> $target" >&2
                MISSING_GOLDENS=$((MISSING_GOLDENS+1))
            fi
        done
    done
    if [ "$MISSING_GOLDENS" -gt 0 ]; then
        if [ "${FORCE_NO_GOLDEN:-0}" = "1" ]; then
            echo "WARN: $MISSING_GOLDENS golden(s) missing; FORCE_NO_GOLDEN=1, continuing." >&2
        else
            echo "FATAL: $MISSING_GOLDENS golden(s) missing; BER>0 cells in those" >&2
            echo "       (scheme, policy) cells would produce meaningless safety stats." >&2
            echo "       Set FORCE_NO_GOLDEN=1 to override." >&2
            exit 1
        fi
    fi
fi

# Phase 2: BER>0 cells. Optional parallelism via JOBS=N. Each background
# worker writes its own index fragment; the parent merges at the end.
PHASE2_T0=$SECONDS
echo ""
echo "=== Phase 2: BER>0 cells (JOBS=$JOBS) ==="
for ber in $BERS; do
    is_ber_zero "$ber" && continue
    for scheme in $SCHEMES; do
        for policy in $POLICIES; do
            if [ "$scheme" = "none" ] && [ "$policy" != "uniform" ]; then
                continue
            fi
            golden_csv="$(golden_path_for "$scheme" "$policy")"
            for fm in $FAULT_MODELS; do
                for due in $DUE_ACTIONS; do
                    for seed in $SEEDS; do
                        if [ "$JOBS" -gt 1 ]; then
                            wait_for_slot "$JOBS"
                            ( run_one "$ber" "$scheme" "$policy" "$seed" "$fm" "$due" "$golden_csv" ) &
                            BG_PIDS+=($!)
                        else
                            run_one "$ber" "$scheme" "$policy" "$seed" "$fm" "$due" "$golden_csv"
                        fi
                    done
                done
            done
        done
    done
done

# Drain remaining background workers.
if [ "$JOBS" -gt 1 ]; then
    wait_for_slot 1
fi
PHASE2_EL=$((SECONDS - PHASE2_T0))

# Merge per-process index fragments into INDEX_CSV. Sort numerically by BER
# then lexically for stable ordering across resume / parallel runs.
shopt -s nullglob
parts=("$INDEX_PART_DIR"/*.csv)
if [ ${#parts[@]} -gt 0 ]; then
    # The header is already in $INDEX_CSV (or was preserved by RESUME=1).
    # Sort by ber (numeric), then by scheme, policy, seed, fm, due (lexical).
    LC_ALL=C sort -t, -k1,1g -k2,2 -k3,3 -k4,4n -k5,5 -k6,6 "${parts[@]}" >> "$INDEX_CSV"
fi
shopt -u nullglob
rm -rf "$INDEX_PART_DIR"

# Summary.
TOTAL_EL=$((PHASE1_EL + PHASE2_EL))
echo
echo "Sweep complete."
echo "  Phase 1 (BER=0):   ${PHASE1_EL}s"
echo "  Phase 2 (BER>0):   ${PHASE2_EL}s"
echo "  Total wallclock:   ${TOTAL_EL}s"
echo "  Index:             $INDEX_CSV"
echo "  Goldens:           $GOLDEN_DIR"
if [ "$MISSING_GOLDENS" -gt 0 ]; then
    echo "  WARN: $MISSING_GOLDENS golden(s) were missing at Phase 2 start; check log above."
fi
echo "Run analyze_ecc_results.py against $OUT_DIR; then make_figures.py against the analysis dir."
