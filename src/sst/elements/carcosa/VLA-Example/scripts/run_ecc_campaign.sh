#!/usr/bin/env bash
# Phase-2 ECC *campaign* driver. Unlike run_ecc_sweep.sh (which factorials
# over BER x scheme x policy x ...), this script runs a small, mechanistic
# matrix where the fault budget is *explicit* and *aimed*: exactly N
# correlated fault events of a chosen mode are injected into a chosen VLA
# kernel, and the resulting unsafe-action / drop / latency outcomes are
# attributed back to that kernel.
#
# The intent is the bottom of Sec. 5 / Fig. 6 in the paper: "if you spend
# your fault budget on KV_CACHE_ATTN under chipkill, what happens?" rather
# than "what is the marginal pressure point at BER=1e-7?".
#
# Each (target_kernel, campaign_mode, scheme, policy, seed) combination
# yields one run with:
#   ECC_FAULT_MODEL=campaign
#   ECC_CAMPAIGN_TARGET_KERNEL=<kernel name or "any">
#   ECC_CAMPAIGN_MODE=<cell|word|row|column|bank|device>
#   ECC_CAMPAIGN_EVENT_BUDGET=<int>
#   ECC_CAMPAIGN_EVENT_RATE=<float in [0,1]>
#
# Defaults run a small but informative grid; override any axis via env.
#
# OUT_DIR layout mirrors run_ecc_sweep.sh (index.csv, run_*.log, goldens/),
# so analyze_ecc_results.py can ingest both pipelines if pointed at the
# same parent. Fault-model="campaign" rows are tagged in index.csv via the
# fm column so downstream filters can distinguish them from sweep rows.

set -u

OUT_DIR="${OUT_DIR:-./ecc_campaign_out}"
mkdir -p "$OUT_DIR"

# Default axes. Each kernel name must exactly match vlaStateName(...) in
# vla-fsm.cc (case-sensitive). "any" runs the campaign without a kernel
# filter, useful as a baseline against the targeted runs.
TARGETS="${TARGETS:-KV_CACHE_ATTN GEMV_PROJECT DECODE_FFN ACTUATE LM_HEAD any}"
CAMPAIGN_MODES="${CAMPAIGN_MODES:-row bank device}"
SCHEMES="${SCHEMES:-secded chipkill}"
POLICIES="${POLICIES:-uniform region_aware full}"
SEEDS="${SEEDS:-1 2 3}"
EVENT_BUDGET="${EVENT_BUDGET:-32}"
EVENT_RATE="${EVENT_RATE:-1.0}"
DUE_ACTION="${DUE_ACTION:-drop_frame}"
PAYLOAD_DTYPE="${PAYLOAD_DTYPE:-bytes}"

CORRECTABLE_PS="${CORRECTABLE_PS:-5000}"
DUE_PS="${DUE_PS:-20000}"
ESCAPE_PS="${ESCAPE_PS:-0}"
SST_CFG="${SST_CFG:-testCarcosaVLA_GPUCPU_Synth.py}"

VLA_PHASE2_MAX_CYCLES="${VLA_PHASE2_MAX_CYCLES:-8}"
VLA_MAX_CYCLES="${VLA_MAX_CYCLES:-$VLA_PHASE2_MAX_CYCLES}"
export VLA_MAX_CYCLES

INDEX_CSV="$OUT_DIR/index.csv"
GOLDEN_DIR="$OUT_DIR/goldens"
mkdir -p "$GOLDEN_DIR"

# Header mirrors run_ecc_sweep.sh's column NAMES exactly (ber, scheme,
# policy, seed, fault_model, due_action, log, exit, golden,
# emitted_golden) so the analyzer's DictReader picks every field up
# without a campaign-specific code path. Campaign-only columns
# (target_kernel, campaign_mode, event_budget, event_rate) are appended
# after; they're absent in plain sweep rows and the analyzer's .get()
# defaults to "" for missing keys, which is exactly what we want.
if [ ! -s "$INDEX_CSV" ]; then
    cat > "$INDEX_CSV" <<EOF
ber,scheme,policy,seed,fault_model,due_action,log,exit,golden,emitted_golden,target_kernel,campaign_mode,event_budget,event_rate
EOF
fi

# Reuse the sweep's golden CSV when one exists (campaign mode injects on
# top of an otherwise BER=0 run, so the BER=0 golden produced by
# run_ecc_sweep.sh is a valid reference). Fall back to capturing one from
# the first campaign cell with EVENT_BUDGET=0 if no sweep was run.
golden_path_for() {
    local p="$GOLDEN_DIR/golden_${1}_${2}.csv"
    if [ -s "$p" ]; then printf '%s' "$p"; return 0; fi
    # Try the sweep's directory if the user pointed us at it.
    local sweep_p="${SWEEP_GOLDEN_DIR:-}/golden_${1}_${2}.csv"
    if [ -n "${SWEEP_GOLDEN_DIR:-}" ] && [ -s "$sweep_p" ]; then
        cp "$sweep_p" "$p" 2>/dev/null || true
        printf '%s' "$p"
        return 0
    fi
    printf ''
}

run_one() {
    local target="$1"
    local cmode="$2"
    local scheme="$3"
    local policy="$4"
    local seed="$5"

    local logname="campaign_${target}_${cmode}_${scheme}_${policy}_seed${seed}.log"
    local logpath="$OUT_DIR/$logname"
    local rcpath="${logpath}.rc"

    if [ -s "$logpath" ] && [ "${RESUME:-0}" = "1" ]; then
        echo "=== campaign target=$target mode=$cmode scheme=$scheme policy=$policy seed=$seed (resume skip) ==="
        return 0
    fi

    echo "=== campaign target=$target mode=$cmode scheme=$scheme policy=$policy seed=$seed -> $logname ==="

    local golden_arg
    golden_arg="$(golden_path_for "$scheme" "$policy")"

    local t0=$SECONDS
    ECC_SCHEME="$scheme" \
    ECC_BER="0" \
    ECC_CORRECTABLE_LATENCY_PS="$CORRECTABLE_PS" \
    ECC_DUE_LATENCY_PS="$DUE_PS" \
    ECC_ESCAPE_LATENCY_PS="$ESCAPE_PS" \
    ECC_KERNEL_POLICY="" \
    ECC_FAULT_MODEL="campaign" \
    ECC_DUE_ACTION="$DUE_ACTION" \
    ECC_PAYLOAD_DTYPE="$PAYLOAD_DTYPE" \
    ECC_CAMPAIGN_TARGET_KERNEL="$target" \
    ECC_CAMPAIGN_MODE="$cmode" \
    ECC_CAMPAIGN_EVENT_BUDGET="$EVENT_BUDGET" \
    ECC_CAMPAIGN_EVENT_RATE="$EVENT_RATE" \
    ECC_SEED="$seed" \
    VLA_RNG_SEED="$seed" \
    VLA_DECODE_EXIT_PROB="${VLA_DECODE_EXIT_PROB:-0.0}" \
    VLA_MAX_CYCLES="$VLA_MAX_CYCLES" \
    VLA_NUM_VIT_LAYERS="${VLA_NUM_VIT_LAYERS:-2}" \
    VLA_NUM_LLM_LAYERS="${VLA_NUM_LLM_LAYERS:-2}" \
    VLA_INITIAL_SEQ_LEN="${VLA_INITIAL_SEQ_LEN:-8}" \
    VLA_MAX_SEQ_LEN="${VLA_MAX_SEQ_LEN:-64}" \
    VLA_NUM_ACTION_TOKENS="${VLA_NUM_ACTION_TOKENS:-1}" \
    VLA_REGIONS="${VLA_REGIONS:-}" \
    ACTION_SCORER_GOLDEN="$golden_arg" \
    ACTION_SCORER_EMIT_GOLDEN=0 \
    env -u VLA_SST_STOP_AT sst "$SST_CFG" > "$logpath" 2>&1
    local rc=$?
    echo "$rc" > "$rcpath"
    local elapsed=$((SECONDS - t0))

    # Reuse the sweep's index.csv column names; ber=0 because campaign
    # mode does not consult ber, and the canonical-slice column for the
    # analyzer is the fault_model string "campaign". Field order:
    # ber, scheme, policy, seed, fault_model, due_action, log, exit,
    # golden, emitted_golden, target_kernel, campaign_mode,
    # event_budget, event_rate.
    local golden_basename=""
    [ -n "$golden_arg" ] && golden_basename="${golden_arg##*/}"
    printf '0,%s,%s,%s,campaign,%s,%s,%d,%s,,%s,%s,%d,%s\n' \
        "$scheme" "$policy" "$seed" \
        "$DUE_ACTION" "$logname" "$rc" \
        "$golden_basename" \
        "$target" "$cmode" "$EVENT_BUDGET" "$EVENT_RATE" \
        >> "$INDEX_CSV"
    # elapsed_s is intentionally not in the index (run_ecc_sweep.sh
    # doesn't carry it either); it's available in the .rc sidecar / log
    # mtimes if a downstream tool wants it.
    : "$elapsed"
}

for target in $TARGETS; do
    for cmode in $CAMPAIGN_MODES; do
        for scheme in $SCHEMES; do
            for policy in $POLICIES; do
                # Skip kernel/region policies under no-ECC: nothing to vary.
                [ "$scheme" = "none" ] && [ "$policy" != "uniform" ] && continue
                for seed in $SEEDS; do
                    run_one "$target" "$cmode" "$scheme" "$policy" "$seed"
                done
            done
        done
    done
done

echo "Campaign driver done. OUT_DIR=$OUT_DIR INDEX_CSV=$INDEX_CSV"
