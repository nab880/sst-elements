#!/usr/bin/env bash
# Phase-2 ECC pressure-point sweep over (BER, scheme, policy_mode, seed).
# Run from VLA-Example/tests/. Requires VLA_BASELINE_CPU_PS / VLA_BASELINE_GPU_PS
# (from extract_baselines.py) and a sourced mysstconfig.sh.

set -u

OUT_DIR="${OUT_DIR:-./ecc_sweep_out}"
mkdir -p "$OUT_DIR"

BERS="${BERS:-1e-12 1e-11 1e-10 1e-9 1e-8 1e-7 1e-6}"
SCHEMES="${SCHEMES:-none secded chipkill}"
POLICIES="${POLICIES:-uniform kernel_aware}"
SEEDS="${SEEDS:-1 2 3 4 5}"
CORRECTABLE_PS="${CORRECTABLE_PS:-5000}"
DUE_PS="${DUE_PS:-20000}"
ESCAPE_PS="${ESCAPE_PS:-0}"
SST_CFG="${SST_CFG:-testCarcosaVLA_GPUCPU_Synth.py}"

# Default kernel-aware policy: harden loop-carried memory-bound kernels; bypass LM_HEAD argmax.
KERNEL_POLICY_AWARE="${KERNEL_POLICY_AWARE:-\
KV_CACHE_ATTN:chipkill:%BER%:8000:30000:0,\
DECODE_FFN:secded:%BER%:5000:20000:0,\
GEMV_PROJECT:secded:%BER%:5000:20000:0,\
LM_HEAD:none:0:0:0:0}"

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
echo "ber,scheme,policy,seed,log,exit" > "$INDEX_CSV"

run_one() {
    local ber="$1"
    local scheme="$2"
    local policy="$3"
    local seed="$4"

    local kernel_policy=""
    if [ "$policy" = "kernel_aware" ]; then
        kernel_policy="${KERNEL_POLICY_AWARE//%BER%/$ber}"
    fi

    local logname="run_${scheme}_${policy}_ber${ber}_seed${seed}.log"
    local logpath="$OUT_DIR/$logname"

    echo "=== ber=$ber scheme=$scheme policy=$policy seed=$seed -> $logname ==="

    ECC_SCHEME="$scheme" \
    ECC_BER="$ber" \
    ECC_CORRECTABLE_LATENCY_PS="$CORRECTABLE_PS" \
    ECC_DUE_LATENCY_PS="$DUE_PS" \
    ECC_ESCAPE_LATENCY_PS="$ESCAPE_PS" \
    ECC_KERNEL_POLICY="$kernel_policy" \
    ECC_SEED="$seed" \
    sst "$SST_CFG" >"$logpath" 2>&1
    local rc=$?

    echo "$ber,$scheme,$policy,$seed,$logname,$rc" >> "$INDEX_CSV"
    if [ $rc -ne 0 ]; then
        echo "  -> exit $rc (see $logpath)"
    fi
}

for ber in $BERS; do
    for scheme in $SCHEMES; do
        for policy in $POLICIES; do
            if [ "$scheme" = "none" ] && [ "$policy" = "kernel_aware" ]; then
                continue
            fi
            for seed in $SEEDS; do
                run_one "$ber" "$scheme" "$policy" "$seed"
            done
        done
    done
done

echo
echo "Sweep complete. Index: $INDEX_CSV"
echo "Run analyze_ecc_results.py against $OUT_DIR to produce figures."
