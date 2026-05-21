#!/usr/bin/env bash
# Section 4 case-study driver: deterministic campaign injection on ACTUATE /
# action_queue with CriticalActionWatcher + scheme/latency Pareto slice.
#
# Usage (from VLA-Example/tests, with VLA_BASELINE_* set):
#   ../scripts/run_case_studies.sh
#   PILOT=1 ../scripts/run_case_studies.sh          # Goldilocks grid (C1/none)
#   PREFLIGHT=1 ../scripts/run_case_studies.sh      # seed=1 acceptance only
#   ../scripts/run_case_studies.sh && \
#     python3 ../scripts/analyze_ecc_results.py ../case_studies --case-studies

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VLA_EX="$(cd "$SCRIPT_DIR/.." && pwd)"
MANIFEST="$VLA_EX/case_studies/manifest.yaml"
OUT_DIR="${OUT_DIR:-$VLA_EX/case_studies}"
mkdir -p "$OUT_DIR/goldens"

SST_CFG="${SST_CFG:-testCarcosaVLA_GPUCPU_Synth.py}"
SEEDS="${SEEDS:-1 2 3}"
SCHEMES="${SCHEMES:-none secded chipkill}"
PREFLIGHT="${PREFLIGHT:-0}"
PILOT="${PILOT:-0}"
RESUME="${RESUME:-0}"

export VLA_DECODE_EXIT_PROB="${VLA_DECODE_EXIT_PROB:-0.0}"
export ECC_FAULT_MODEL=campaign
export ECC_DUE_ACTION=drop_frame
export ECC_BER=0
export CRITICAL_ACTION_WATCHER=1
export CRITICAL_WATCHER_REGION=action_queue
export CRITICAL_WATCHER_LEN=64
export ACTION_SCORER=1
export ACTION_SCORER_GOLDEN_REQUIRED=1

REGIONS_CSV="${REGIONS_CSV:-weights:0x10000000:0x4000000,kv_cache:0x14000000:0x100000,activations:0x14100000:0x200000,action_queue:0x14300000:0x1000}"
KERNEL_POLICY_AWARE="${KERNEL_POLICY_AWARE:-KV_CACHE_ATTN:chipkill:0:8000:30000:0,DECODE_FFN:secded:0:5000:20000:0,GEMV_PROJECT:secded:0:5000:20000:0,LM_HEAD:none:0:0:0:0}"
export VLA_REGIONS="$REGIONS_CSV"
export ECC_KERNEL_POLICY="$KERNEL_POLICY_AWARE"

eval "$(python3 "$SCRIPT_DIR/load_case_manifest.py" --shell-defaults 2>/dev/null)" || true
VLA_PHASE2_MAX_CYCLES="${VLA_PHASE2_MAX_CYCLES:-8}"
export VLA_MAX_CYCLES="${VLA_MAX_CYCLES:-$VLA_PHASE2_MAX_CYCLES}"

export ECC_CAMPAIGN_TARGET_KERNEL="${ECC_CAMPAIGN_TARGET_KERNEL:-ACTUATE}"
export ECC_ADDR_FILTER_REGION="${ECC_ADDR_FILTER_REGION:-action_queue}"
export ECC_ADDR_FILTER_LEN="${ECC_ADDR_FILTER_LEN:-64}"
export ECC_CAMPAIGN_MAX_PER_KERNEL_ENTRY="${ECC_CAMPAIGN_MAX_PER_KERNEL_ENTRY:-1}"
CASE_DEFAULT_BUDGET="${CASE_DEFAULT_BUDGET:-8}"
CASE_DEFAULT_RATE="${CASE_DEFAULT_RATE:-1.0}"

INDEX_CSV="$OUT_DIR/index.csv"
if [ ! -s "$INDEX_CSV" ]; then
    echo "case_id,scheme,policy,latency_profile,seed,fault_model,due_action,log,exit,golden,campaign_mode,event_budget,event_rate,errors_fixed" > "$INDEX_CSV"
fi

golden_path_for() {
    local p="$OUT_DIR/goldens/golden_${1}_kernel_aware.csv"
    [ -s "$p" ] && printf '%s' "$p"
}

case_params() {
    python3 - "$1" "$MANIFEST" <<'PY'
import json, subprocess, sys
case_id, manifest = sys.argv[1], sys.argv[2]
subprocess.check_call([sys.executable, "-c", "pass"])  # placeholder
PY
}

# Read one case block from manifest via inline python
read_case() {
    local cid="$1"
    python3 "$SCRIPT_DIR/load_case_manifest.py" --case "$cid" | python3 -c "
import json,sys
c=json.load(sys.stdin)
print('mode=%s' % c['mode'])
print('budget=%s' % c['budget'])
print('fixed=%s' % c['fixed'])
print('multi=%s' % c['multi'])
"
}

latency_ps() {
    local profile="$1"
    python3 "$SCRIPT_DIR/load_case_manifest.py" | python3 -c "
import json,sys
m=json.load(sys.stdin)
p=m['latency_profiles'][sys.argv[1]]
print(p['correctable_ps'], p['due_ps'])
" "$profile"
}

run_one() {
    local case_id="$1" scheme="$2" policy="$3" lat_prof="$4" seed="$5"

    eval "$(read_case "$case_id")"
    local mode="${mode:-cell}"
    local fixed="${fixed:-0}"
    local multi="${multi:-false}"
    local budget="${6:-${budget:-$CASE_DEFAULT_BUDGET}}"
    local rate="${7:-$CASE_DEFAULT_RATE}"

    local corr due
    read -r corr due < <(latency_ps "${lat_prof:-default}")

    local logname="case_${case_id}_${scheme}_${policy}_${lat_prof}_seed${seed}.log"
    local logpath="$OUT_DIR/$logname"

    if [ "$RESUME" = "1" ] && [ -s "$logpath" ]; then
        echo "=== $case_id $scheme $policy $lat_prof seed=$seed (resume skip) ==="
        return 0
    fi

    local golden
    golden="$(golden_path_for "$scheme")"
    if [ "$case_id" = "C0" ] && [ -z "$golden" ]; then
        export ACTION_SCORER_EMIT_GOLDEN=1
    else
        export ACTION_SCORER_EMIT_GOLDEN=0
    fi

    echo "=== case=$case_id scheme=$scheme policy=$policy lat=$lat_prof seed=$seed -> $logname ==="

    local multi_flag=0
    [ "$multi" = "True" ] || [ "$multi" = "true" ] && multi_flag=1

    ECC_SCHEME="$scheme" \
    ECC_KERNEL_POLICY="$(echo "$KERNEL_POLICY_AWARE" | sed 's/%BER%/0/g')" \
    ECC_CAMPAIGN_MODE="$mode" \
    ECC_CAMPAIGN_EVENT_BUDGET="$budget" \
    ECC_CAMPAIGN_EVENT_RATE="$rate" \
    ECC_CAMPAIGN_ERRORS_FIXED="$fixed" \
    ECC_CAMPAIGN_FORCE_MULTI_CHIP="$multi_flag" \
    ECC_CORRECTABLE_LATENCY_PS="$corr" \
    ECC_DUE_LATENCY_PS="$due" \
    ECC_SEED="$seed" \
    VLA_RNG_SEED="$seed" \
    ACTION_SCORER_GOLDEN="${golden:-}" \
    env -u VLA_SST_STOP_AT sst "$SST_CFG" > "$logpath" 2>&1
    local rc=$?

    if [ "$case_id" = "C0" ] && [ "$rc" -eq 0 ] && [ ! -s "$OUT_DIR/goldens/golden_${scheme}_kernel_aware.csv" ]; then
        python3 "$SCRIPT_DIR/analyze_ecc_results.py" "$OUT_DIR" --min-events 0 --min-frames 1 2>/dev/null || true
        grep -A200 "Golden Emit" "$logpath" 2>/dev/null | tail -n +2 | grep -E '^[0-9]' > \
            "$OUT_DIR/goldens/golden_${scheme}_kernel_aware.csv" 2>/dev/null || true
    fi

    printf '%s,%s,%s,%s,%s,campaign,drop_frame,%s,%d,%s,%s,%s,%s,%s\n' \
        "$case_id" "$scheme" "$policy" "$lat_prof" "$seed" \
        "$logname" "$rc" "${golden##*/}" "$mode" "$budget" "$rate" "$fixed" \
        >> "$INDEX_CSV"
}

cd "$VLA_EX/tests" 2>/dev/null || cd "$(dirname "$SST_CFG")" 2>/dev/null || true
TESTS_DIR="$(pwd)"
export SST_LIB_PATH="${SST_LIB_PATH:-}"

if [ "$PILOT" = "1" ]; then
    echo "Pilot Goldilocks grid (C1, scheme=none, seed=1)"
    for budget in 4 8 12; do
        for rate in 0.25 0.5 1.0; do
            run_one C1 none kernel_aware default 1 "$budget" "$rate"
        done
    done
    echo "Pilot done. Inspect logs under $OUT_DIR and unsafe_action_rate in summaries."
    exit 0
fi

if [ "$PREFLIGHT" = "1" ]; then
    SEEDS="1"
fi

# C0 golden for each scheme
for scheme in $SCHEMES; do
    [ "$scheme" = "none" ] && pol=uniform || pol=kernel_aware
    run_one C0 "$scheme" "$pol" default 1
done

for case_id in C1 C2 C3 C4; do
    for scheme in $SCHEMES; do
        if [ "$scheme" = "none" ]; then pol=uniform; else pol=kernel_aware; fi
        for seed in $SEEDS; do
            run_one "$case_id" "$scheme" "$pol" default "$seed"
        done
    done
done

# Pareto slice: C2 x secded x latency profiles
for lat in fast default strict; do
    for seed in $SEEDS; do
        run_one C2 secded kernel_aware "$lat" "$seed"
    done
done

echo "Case studies done. OUT_DIR=$OUT_DIR"
echo "  python3 $SCRIPT_DIR/analyze_ecc_results.py $OUT_DIR --case-studies"
echo "  python3 $SCRIPT_DIR/make_figures.py $OUT_DIR/analysis --case-studies"
