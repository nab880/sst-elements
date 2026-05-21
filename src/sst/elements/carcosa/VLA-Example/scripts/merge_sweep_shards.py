#!/usr/bin/env python3
"""Merge SLURM-sharded ECC sweep outputs into one run directory + analysis.

After shard_run_ecc_sweep.sh finishes, each shard writes:
  OUT_ROOT/shard_<i>/ecc_sweep_out/{index.csv, run_*.log, goldens/}

This script either:
  (A) merges raw run dirs (default), then optionally re-runs analyze_ecc_results.py
  (B) merges existing per-shard analysis/ trees (--analysis-only)

Examples:
  # Merge logs + index, then analyze once on the union:
  python3 merge_sweep_shards.py ./ecc_shard_out_20260517 \\
      --analyze --canonical-slice jedec_mix+drop_frame

  # Shards were already analyzed individually; pool CSVs only:
  python3 merge_sweep_shards.py ./ecc_shard_out_20260517 --analysis-only \\
      --canonical-slice jedec_mix+drop_frame
"""

from __future__ import annotations

import argparse
import os
import sys

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if _SCRIPT_DIR not in sys.path:
    sys.path.insert(0, _SCRIPT_DIR)

from ecc_shard_merge_lib import (  # noqa: E402
    discover_shard_leaf_dirs,
    merge_analysis_from_shards,
    merge_raw_run_dirs,
    run_analyzer,
)

LEAF = "ecc_sweep_out"
INDEX_KEY = ("ber", "scheme", "policy", "seed", "fault_model", "due_action", "log")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out_root", help="OUT_ROOT passed to shard_run_ecc_sweep.sh")
    ap.add_argument("-o", "--merged-dir", default=None,
                    help=f"Merged run dir (default: <out_root>/{LEAF}_merged)")
    ap.add_argument("--analysis-only", action="store_true",
                    help="Skip raw merge; concat shard_*/ecc_sweep_out/analysis only")
    ap.add_argument("--analyze", action="store_true",
                    help="After raw merge, run analyze_ecc_results.py on merged dir")
    ap.add_argument("--canonical-slice", default="",
                    help="e.g. jedec_mix+drop_frame (forwarded to analyzer / slice step)")
    ap.add_argument("--unsafe-budget", type=float, default=1e-6)
    ap.add_argument("--policy-contrasts", action="store_true")
    ap.add_argument("--deadline-ms", type=float, default=33.0)
    ap.add_argument("--dram-capacity-mb", type=float, default=1024.0)
    ap.add_argument("--sim-ns-per-event", type=float, default=100.0)
    ap.add_argument("--line-bits", type=int, default=512)
    args = ap.parse_args()

    out_root = os.path.abspath(args.out_root)
    if not os.path.isdir(out_root):
        print(f"ERROR: {out_root} is not a directory", file=sys.stderr)
        return 2

    shard_dirs = discover_shard_leaf_dirs(out_root, LEAF)
    if not shard_dirs:
        print(f"ERROR: no shard_*/{LEAF} under {out_root}", file=sys.stderr)
        return 2

    merged_dir = os.path.abspath(
        args.merged_dir or os.path.join(out_root, f"{LEAF}_merged"))
    merged_analysis = os.path.join(merged_dir, "analysis")

    if args.analysis_only:
        shard_analysis = [os.path.join(d, "analysis") for d in shard_dirs]
        shard_analysis = [d for d in shard_analysis if os.path.isdir(d)]
        if not shard_analysis:
            print("ERROR: no shard analysis/ directories found. "
                  "Run analyze_ecc_results.py per shard first, or omit --analysis-only.",
                  file=sys.stderr)
            return 2
        print(f"Merging analysis from {len(shard_analysis)} shard(s) -> {merged_analysis}")
        merge_analysis_from_shards(
            shard_analysis,
            merged_analysis,
            deadline_ms=args.deadline_ms,
            dram_capacity_mb=args.dram_capacity_mb,
            sim_ns_per_event=args.sim_ns_per_event,
            line_bits=args.line_bits,
            canonical_slice=args.canonical_slice,
        )
        print(f"Done. Merged analysis: {merged_analysis}")
        return 0

    print(f"Merging {len(shard_dirs)} shard run dir(s) -> {merged_dir}")
    n_idx, n_logs, warnings = merge_raw_run_dirs(
        shard_dirs, merged_dir, index_key_cols=INDEX_KEY)
    for w in warnings:
        print(f"  WARN: {w}")
    print(f"  index rows: {n_idx}, log symlinks: {n_logs}")

    if args.analyze:
        analyzer = os.path.join(_SCRIPT_DIR, "analyze_ecc_results.py")
        extra = []
        if args.canonical_slice:
            extra.extend(["--canonical-slice", args.canonical_slice])
        if args.unsafe_budget is not None:
            extra.extend(["--unsafe-budget", str(args.unsafe_budget)])
        if args.policy_contrasts:
            extra.append("--policy-contrasts")
        extra.extend([
            "--deadline-ms", str(args.deadline_ms),
            "--dram-capacity-mb", str(args.dram_capacity_mb),
            "--sim-ns-per-event", str(args.sim_ns_per_event),
            "--line-bits", str(args.line_bits),
        ])
        rc = run_analyzer(merged_dir, merged_analysis, analyzer, extra)
        if rc != 0:
            return rc
        print(f"Analysis written to {merged_analysis}")
    else:
        print("Next: analyze the merged run dir, e.g.")
        print(f"  python3 {_SCRIPT_DIR}/analyze_ecc_results.py {merged_dir}")
        print("Or re-run this script with --analyze")

    return 0


if __name__ == "__main__":
    sys.exit(main())
