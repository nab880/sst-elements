#!/usr/bin/env bash
# shard_run_ecc_sweep.sh
#
# SLURM wrapper that sbatches N (default 4) jobs, each of which runs the
# entire run_ecc_sweep.sh on a disjoint subset of SEEDS. Seeds are the
# only sweep axis where partitioning is safe without touching the inner
# script: the BER=0 cell is regenerated independently per shard (each
# shard writes its own goldens) and the analyzer can merge later by
# pointing at the union of shard OUT_DIRs.
#
# Each sbatch job lands in $OUT_ROOT/shard_<i>/ecc_sweep_out and runs
# run_ecc_sweep.sh with SEEDS overridden. All other axes (BERS, SCHEMES,
# POLICIES, FAULT_MODELS, DUE_ACTIONS, ...) are inherited from the
# caller's environment so the shards see exactly the same matrix the
# user would have passed to run_ecc_sweep.sh directly.
#
# Usage:
#   sbatch-account / partition / time / cpus / mem are SLURM-side knobs
#   exposed via env vars below. Shard count defaults to 4; override with
#   SHARD_COUNT=N. Pass --dry-run to print the sbatch invocations
#   without submitting.
#
# Merge step (after all shards finish):
#   python3 merge_sweep_shards.py "$OUT_ROOT" --analyze \
#       --canonical-slice jedec_mix+drop_frame
#   Or pool pre-analyzed shards: merge_sweep_shards.py "$OUT_ROOT" --analysis-only ...

set -u

SHARD_COUNT="${SHARD_COUNT:-4}"
OUT_ROOT="${OUT_ROOT:-./ecc_shard_out_$(date +%Y%m%d_%H%M%S)}"
SEEDS_FULL="${SEEDS:-1 2 3 4 5 6 7 8}"

# SLURM resource knobs. Sized for a typical campus-cluster node; override
# any of them per submission. Job name carries the shard id for sacct.
SLURM_PARTITION="${SLURM_PARTITION:-compute}"
SLURM_ACCOUNT="${SLURM_ACCOUNT:-}"
SLURM_TIME="${SLURM_TIME:-24:00:00}"
SLURM_CPUS="${SLURM_CPUS:-8}"
SLURM_MEM="${SLURM_MEM:-32G}"
SLURM_JOB_PREFIX="${SLURM_JOB_PREFIX:-ecc_shard}"

DRY_RUN=0
for arg in "$@"; do
    case "$arg" in
        --dry-run|-n) DRY_RUN=1 ;;
        -h|--help)
            sed -n '2,30p' "$0"
            exit 0
            ;;
        *) echo "unknown arg '$arg'" >&2; exit 1 ;;
    esac
done

# Validate environment.
if ! command -v sbatch >/dev/null 2>&1 && [ "$DRY_RUN" != "1" ]; then
    echo "ERROR: 'sbatch' not on PATH (use --dry-run to preview)." >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INNER_SCRIPT="$SCRIPT_DIR/run_ecc_sweep.sh"
if [ ! -x "$INNER_SCRIPT" ]; then
    echo "ERROR: $INNER_SCRIPT not found or not executable." >&2
    exit 1
fi

mkdir -p "$OUT_ROOT"

# Round-robin partition $SEEDS_FULL into $SHARD_COUNT disjoint lists.
# (Round-robin instead of contiguous chunking so each shard has a mix of
# low/high seed values; statistically unimportant but operationally
# avoids the case where shard 0 gets all "fast" seeds.)
declare -a SHARD_SEEDS
for ((i=0; i<SHARD_COUNT; i++)); do SHARD_SEEDS[i]=""; done
i=0
for s in $SEEDS_FULL; do
    idx=$((i % SHARD_COUNT))
    if [ -z "${SHARD_SEEDS[$idx]}" ]; then
        SHARD_SEEDS[$idx]="$s"
    else
        SHARD_SEEDS[$idx]="${SHARD_SEEDS[$idx]} $s"
    fi
    i=$((i+1))
done

submit_one() {
    local shard_id="$1"
    local seeds_for_shard="$2"
    local shard_dir="$OUT_ROOT/shard_${shard_id}"
    mkdir -p "$shard_dir"

    # Per-shard wrapper script: SLURM execs this on the compute node. The
    # wrapper re-exports the parent environment knobs we care about and
    # then chains to run_ecc_sweep.sh. Generated freshly per submission
    # so the seeds list and OUT_DIR are baked in (avoids relying on
    # SLURM env propagation for those critical values).
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
export SEEDS="$seeds_for_shard"
export OUT_DIR="$shard_dir/ecc_sweep_out"
# Inherit (or default) every other knob the inner script consumes; the
# shard does not narrow the matrix on any axis other than seeds.
${BERS:+export BERS=\"$BERS\"}
${SCHEMES:+export SCHEMES=\"$SCHEMES\"}
${POLICIES:+export POLICIES=\"$POLICIES\"}
${FAULT_MODELS:+export FAULT_MODELS=\"$FAULT_MODELS\"}
${DUE_ACTIONS:+export DUE_ACTIONS=\"$DUE_ACTIONS\"}
export PAYLOAD_DTYPE="\${PAYLOAD_DTYPE:-${PAYLOAD_DTYPE:-bytes}}"
${VLA_BASELINE_CPU_PS:+export VLA_BASELINE_CPU_PS=\"$VLA_BASELINE_CPU_PS\"}
${VLA_BASELINE_GPU_PS:+export VLA_BASELINE_GPU_PS=\"$VLA_BASELINE_GPU_PS\"}
export VLA_PHASE2_MAX_CYCLES="\${VLA_PHASE2_MAX_CYCLES:-${VLA_PHASE2_MAX_CYCLES:-8}}"
export JOBS="\${JOBS:-${JOBS:-$SLURM_CPUS}}"
export LOG_FILTER="\${LOG_FILTER:-${LOG_FILTER:-1}}"
exec "$INNER_SCRIPT"
EOF
    chmod +x "$wrapper"

    if [ "$DRY_RUN" = "1" ]; then
        echo "[dry-run] sbatch $wrapper"
        echo "          SEEDS='$seeds_for_shard'"
        echo "          OUT_DIR=$shard_dir/ecc_sweep_out"
    else
        local jid
        jid="$(sbatch --parsable "$wrapper")"
        echo "shard $shard_id submitted: jobid=$jid seeds='$seeds_for_shard'"
    fi
}

echo "Sharded sweep -> $OUT_ROOT (SHARD_COUNT=$SHARD_COUNT)"
for ((i=0; i<SHARD_COUNT; i++)); do
    if [ -z "${SHARD_SEEDS[$i]}" ]; then
        echo "shard $i: empty seed list (SEEDS_FULL='$SEEDS_FULL' too small) -- skipping"
        continue
    fi
    submit_one "$i" "${SHARD_SEEDS[$i]}"
done

echo ""
echo "After all shards finish, merge and analyze:"
echo "  python3 $SCRIPT_DIR/merge_sweep_shards.py \"$OUT_ROOT\" --analyze \\"
echo "      --canonical-slice jedec_mix+drop_frame"
echo "  python3 $SCRIPT_DIR/make_figures.py \"$OUT_ROOT/ecc_sweep_out_merged/analysis\""
