#!/usr/bin/env bash
# shard_run_ecc_campaign.sh
#
# SLURM wrapper that sbatches N (default 4) jobs, each running the entire
# run_ecc_campaign.sh on a disjoint subset of TARGETS (the list of VLA
# kernels into which the fault budget is aimed). Targets are the natural
# parallelism axis for the campaign workflow:
#
#   - Each target tells a separate mechanistic story ("what happens when
#     we burn N row-faults on KV_CACHE_ATTN") so the per-shard outputs
#     are individually meaningful, not just intermediate splits.
#   - Targets are independent: the fault budget, the gating predicate,
#     and the resulting per-frame attribution are all keyed off the
#     target alone, so two shards never write to the same goldens/ or
#     index.csv row.
#   - The default TARGETS list has 6 entries; SHARD_COUNT=4 partitions
#     round-robin into [0:KV_CACHE_ATTN+LM_HEAD], [1:GEMV_PROJECT+any],
#     [2:DECODE_FFN], [3:ACTUATE]. Override SHARD_COUNT to taste.
#
# Each sbatch job lands in $OUT_ROOT/shard_<i>/ecc_campaign_out and runs
# run_ecc_campaign.sh with TARGETS overridden. Every other axis
# (CAMPAIGN_MODES, SCHEMES, POLICIES, SEEDS, EVENT_BUDGET, EVENT_RATE,
# DUE_ACTION, ...) is inherited from the caller's environment so the
# shards see exactly the same matrix the user would have passed to
# run_ecc_campaign.sh directly.
#
# Usage:
#   SHARD_COUNT=4 ./shard_run_ecc_campaign.sh           # submit
#   ./shard_run_ecc_campaign.sh --dry-run               # preview
#
#   Override targets:
#   TARGETS="KV_CACHE_ATTN GEMV_PROJECT" ./shard_run_ecc_campaign.sh
#
#   Reuse goldens captured by a prior run_ecc_sweep.sh:
#   SWEEP_GOLDEN_DIR=/path/to/ecc_sweep_out/goldens \
#       ./shard_run_ecc_campaign.sh
#
# Merge step (after all shards finish):
#   python3 merge_campaign_shards.py "$OUT_ROOT" --analyze

set -u

SHARD_COUNT="${SHARD_COUNT:-4}"
OUT_ROOT="${OUT_ROOT:-./ecc_campaign_shard_out_$(date +%Y%m%d_%H%M%S)}"
TARGETS_FULL="${TARGETS:-KV_CACHE_ATTN GEMV_PROJECT DECODE_FFN ACTUATE LM_HEAD any}"

# SLURM resource knobs. Campaign cells are typically smaller than sweep
# cells (one ber=0 run per target, no factorial over BERS), so the
# defaults shrink the wall-clock and CPU asks. Override per submission.
SLURM_PARTITION="${SLURM_PARTITION:-compute}"
SLURM_ACCOUNT="${SLURM_ACCOUNT:-}"
SLURM_TIME="${SLURM_TIME:-08:00:00}"
SLURM_CPUS="${SLURM_CPUS:-4}"
SLURM_MEM="${SLURM_MEM:-16G}"
SLURM_JOB_PREFIX="${SLURM_JOB_PREFIX:-ecc_campaign_shard}"

DRY_RUN=0
for arg in "$@"; do
    case "$arg" in
        --dry-run|-n) DRY_RUN=1 ;;
        -h|--help)
            sed -n '2,40p' "$0"
            exit 0
            ;;
        *) echo "unknown arg '$arg'" >&2; exit 1 ;;
    esac
done

if ! command -v sbatch >/dev/null 2>&1 && [ "$DRY_RUN" != "1" ]; then
    echo "ERROR: 'sbatch' not on PATH (use --dry-run to preview)." >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INNER_SCRIPT="$SCRIPT_DIR/run_ecc_campaign.sh"
if [ ! -x "$INNER_SCRIPT" ]; then
    echo "ERROR: $INNER_SCRIPT not found or not executable." >&2
    exit 1
fi

mkdir -p "$OUT_ROOT"

# Round-robin partition $TARGETS_FULL into $SHARD_COUNT disjoint lists.
# Round-robin (rather than contiguous chunking) so a shard never gets
# only "any" or only the LLM-tail kernels - keeps wall-clock per shard
# comparable.
declare -a SHARD_TARGETS
for ((i=0; i<SHARD_COUNT; i++)); do SHARD_TARGETS[i]=""; done
i=0
for t in $TARGETS_FULL; do
    idx=$((i % SHARD_COUNT))
    if [ -z "${SHARD_TARGETS[$idx]}" ]; then
        SHARD_TARGETS[$idx]="$t"
    else
        SHARD_TARGETS[$idx]="${SHARD_TARGETS[$idx]} $t"
    fi
    i=$((i+1))
done

submit_one() {
    local shard_id="$1"
    local targets_for_shard="$2"
    local shard_dir="$OUT_ROOT/shard_${shard_id}"
    mkdir -p "$shard_dir"

    # Per-shard wrapper script (generated freshly so the targets list
    # and OUT_DIR are baked in; SLURM env propagation can be lossy).
    local wrapper="$shard_dir/run_shard.sh"
    cat > "$wrapper" <<EOF
#!/usr/bin/env bash
#SBATCH --job-name=${SLURM_JOB_PREFIX}_${shard_id}
#SBATCH --output=$shard_dir/slurm.out
#SBATCH --error=$shard_dir/slurm.err
#SBATCH --partition=$SLURM_PARTITION
${SLURM_ACCOUNT:+#SBATCH --account=$SLURM_ACCOUNT}
#SBATCH --time=$SLURM_TIME
#SBATCH --cpus-per-task=$SLURM_CPUS
#SBATCH --mem=$SLURM_MEM
set -u
cd "$shard_dir"
export TARGETS="$targets_for_shard"
export OUT_DIR="$shard_dir/ecc_campaign_out"
# Inherit (or default) every other knob run_ecc_campaign.sh consumes;
# the shard does not narrow the matrix on any axis other than targets.
${CAMPAIGN_MODES:+export CAMPAIGN_MODES=\"$CAMPAIGN_MODES\"}
${SCHEMES:+export SCHEMES=\"$SCHEMES\"}
${POLICIES:+export POLICIES=\"$POLICIES\"}
${SEEDS:+export SEEDS=\"$SEEDS\"}
export EVENT_BUDGET="\${EVENT_BUDGET:-${EVENT_BUDGET:-32}}"
export EVENT_RATE="\${EVENT_RATE:-${EVENT_RATE:-1.0}}"
export DUE_ACTION="\${DUE_ACTION:-${DUE_ACTION:-drop_frame}}"
export PAYLOAD_DTYPE="\${PAYLOAD_DTYPE:-${PAYLOAD_DTYPE:-bytes}}"
${VLA_BASELINE_CPU_PS:+export VLA_BASELINE_CPU_PS=\"$VLA_BASELINE_CPU_PS\"}
${VLA_BASELINE_GPU_PS:+export VLA_BASELINE_GPU_PS=\"$VLA_BASELINE_GPU_PS\"}
export VLA_PHASE2_MAX_CYCLES="\${VLA_PHASE2_MAX_CYCLES:-${VLA_PHASE2_MAX_CYCLES:-8}}"
${VLA_REGIONS:+export VLA_REGIONS=\"$VLA_REGIONS\"}
# Optional: point each shard at the sweep's golden_*.csv directory so
# campaign cells can compare against the same fault-free reference the
# sweep used. Empty -> the campaign runs without scorer comparison.
export SWEEP_GOLDEN_DIR="\${SWEEP_GOLDEN_DIR:-${SWEEP_GOLDEN_DIR:-}}"
exec "$INNER_SCRIPT"
EOF
    chmod +x "$wrapper"

    if [ "$DRY_RUN" = "1" ]; then
        echo "[dry-run] sbatch $wrapper"
        echo "          TARGETS='$targets_for_shard'"
        echo "          OUT_DIR=$shard_dir/ecc_campaign_out"
    else
        local jid
        jid="$(sbatch --parsable "$wrapper")"
        echo "shard $shard_id submitted: jobid=$jid targets='$targets_for_shard'"
    fi
}

echo "Sharded campaign -> $OUT_ROOT (SHARD_COUNT=$SHARD_COUNT)"
for ((i=0; i<SHARD_COUNT; i++)); do
    if [ -z "${SHARD_TARGETS[$i]}" ]; then
        echo "shard $i: empty target list (TARGETS_FULL='$TARGETS_FULL' too small) -- skipping"
        continue
    fi
    submit_one "$i" "${SHARD_TARGETS[$i]}"
done

echo ""
echo "After all shards finish, merge and analyze:"
echo "  python3 $SCRIPT_DIR/merge_campaign_shards.py \"$OUT_ROOT\" --analyze"
echo "  python3 $SCRIPT_DIR/make_figures.py \"$OUT_ROOT/ecc_campaign_out_merged/analysis\""
