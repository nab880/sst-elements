#!/usr/bin/env bash
# Phase-1 ECC spot-check: runs real VLA binaries at a few (BER, scheme, policy)
# points. EccGuard escape counters in the log act as the simulator-side SDC
# proxy. Run from VLA-Example/tests/ after sourcing mysstconfig.sh.

set -u

OUT_DIR="${OUT_DIR:-./ecc_phase1_out}"
mkdir -p "$OUT_DIR"

BERS="${BERS:-1e-9 1e-8 1e-7}"
SCHEMES="${SCHEMES:-secded}"
POLICIES="${POLICIES:-uniform kernel_aware}"
SEEDS="${SEEDS:-1 2 3}"
CORRECTABLE_PS="${CORRECTABLE_PS:-5000}"
DUE_PS="${DUE_PS:-20000}"
ESCAPE_PS="${ESCAPE_PS:-0}"
SST_CFG="${SST_CFG:-testCarcosaVLA_GPUCPU.py}"
GOLDEN="${GOLDEN:-1}"

KERNEL_POLICY_AWARE="${KERNEL_POLICY_AWARE:-\
KV_CACHE_ATTN:chipkill:%BER%:8000:30000:0,\
DECODE_FFN:secded:%BER%:5000:20000:0,\
GEMV_PROJECT:secded:%BER%:5000:20000:0,\
LM_HEAD:none:0:0:0:0}"

if ! command -v sst >/dev/null 2>&1; then
    echo "ERROR: 'sst' not on PATH; did you source mysstconfig.sh?" >&2
    exit 1
fi
if [ ! -x ./vla_cpu ] || [ ! -x ./vla_gpu ]; then
    echo "ERROR: ./vla_cpu and ./vla_gpu must be built and present in cwd." >&2
    echo "Hint: see VLA-Example/README.md for the riscv64-linux-gnu-gcc build." >&2
    exit 1
fi

INDEX_CSV="$OUT_DIR/index.csv"
echo "ber,scheme,policy,seed,log,exit" > "$INDEX_CSV"

run_one() {
    local label="$1"
    local ber="$2"
    local scheme="$3"
    local policy="$4"
    local seed="$5"

    local kernel_policy=""
    if [ "$policy" = "kernel_aware" ]; then
        kernel_policy="${KERNEL_POLICY_AWARE//%BER%/$ber}"
    fi

    local logname="phase1_${label}.log"
    local logpath="$OUT_DIR/$logname"

    echo "=== $label (ber=$ber scheme=$scheme policy=$policy seed=$seed) ==="

    env -u VLA_SST_STOP_AT \
    ECC_SCHEME="$scheme" \
    ECC_BER="$ber" \
    ECC_CORRECTABLE_LATENCY_PS="$CORRECTABLE_PS" \
    ECC_DUE_LATENCY_PS="$DUE_PS" \
    ECC_ESCAPE_LATENCY_PS="$ESCAPE_PS" \
    ECC_KERNEL_POLICY="$kernel_policy" \
    ECC_SEED="$seed" \
    VLA_MAX_CYCLES="${VLA_MAX_CYCLES:-${VLA_PHASE2_MAX_CYCLES:-1}}" \
    VLA_NUM_VIT_LAYERS="${VLA_NUM_VIT_LAYERS:-2}" \
    VLA_NUM_LLM_LAYERS="${VLA_NUM_LLM_LAYERS:-2}" \
    VLA_INITIAL_SEQ_LEN="${VLA_INITIAL_SEQ_LEN:-8}" \
    VLA_MAX_SEQ_LEN="${VLA_MAX_SEQ_LEN:-64}" \
    VLA_NUM_ACTION_TOKENS="${VLA_NUM_ACTION_TOKENS:-1}" \
    sst "$SST_CFG" >"$logpath" 2>&1
    local rc=$?

    echo "$ber,$scheme,$policy,$seed,$logname,$rc" >> "$INDEX_CSV"
    if [ $rc -ne 0 ]; then
        echo "  -> exit $rc (see $logpath)"
    fi
}

# Golden baseline (no faults, no ECC) for downstream latency comparison.
if [ "$GOLDEN" = "1" ]; then
    run_one "golden" "0.0" "none" "uniform" "0"
fi

for ber in $BERS; do
    for scheme in $SCHEMES; do
        for policy in $POLICIES; do
            if [ "$scheme" = "none" ] && [ "$policy" = "kernel_aware" ]; then
                continue
            fi
            for seed in $SEEDS; do
                label="${scheme}_${policy}_ber${ber}_seed${seed}"
                run_one "$label" "$ber" "$scheme" "$policy" "$seed"
            done
        done
    done
done

echo
echo "Spot-check complete. Index: $INDEX_CSV"
echo "Run analyze_ecc_results.py against $OUT_DIR for the SDC summary."
