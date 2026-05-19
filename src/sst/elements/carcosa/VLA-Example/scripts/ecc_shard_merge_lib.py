#!/usr/bin/env python3
"""Shared helpers for merge_sweep_shards.py and merge_campaign_shards.py."""

from __future__ import annotations

import csv
import glob
import math
import os
import shutil
import subprocess
import sys
from collections import defaultdict
from typing import Dict, Iterable, List, Optional, Sequence, Tuple


def discover_shard_leaf_dirs(out_root: str, leaf_name: str) -> List[str]:
    """Return sorted shard_*/<leaf_name> directories under out_root."""
    pattern = os.path.join(out_root, "shard_*", leaf_name)
    dirs = [p for p in glob.glob(pattern) if os.path.isdir(p)]
    return sorted(dirs)


def read_csv(path: str) -> List[Dict[str, str]]:
    if not os.path.isfile(path):
        return []
    with open(path, "r", newline="") as f:
        return list(csv.DictReader(f))


def write_csv(path: str, rows: List[Dict]) -> None:
    if not rows:
        return
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    # Union fieldnames in first-seen order across rows.
    fields: List[str] = []
    seen = set()
    for row in rows:
        for k in row.keys():
            if k not in seen:
                seen.add(k)
                fields.append(k)
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)


def concat_csv_files(paths: Sequence[str]) -> List[Dict[str, str]]:
    out: List[Dict[str, str]] = []
    for p in paths:
        out.extend(read_csv(p))
    return out


def wilson_ci(k: int, n: int, z: float = 1.959963984540054) -> Tuple[float, float, float]:
    if n <= 0:
        return float("nan"), float("nan"), float("nan")
    k = max(0, min(k, n))
    p = k / n
    z2 = z * z
    denom = 1.0 + z2 / n
    center = (p + z2 / (2.0 * n)) / denom
    half = (z / denom) * math.sqrt(p * (1.0 - p) / n + z2 / (4.0 * n * n))
    return p, max(0.0, center - half), min(1.0, center + half)


def _as_int(v) -> int:
    if v in ("", None):
        return 0
    try:
        return int(float(v))
    except (TypeError, ValueError):
        return 0


def percentile(vals: List[int], p: float) -> float:
    if not vals:
        return float("nan")
    s = sorted(vals)
    if len(s) == 1:
        return float(s[0])
    k = (len(s) - 1) * (p / 100.0)
    lo = int(math.floor(k))
    hi = int(math.ceil(k))
    if lo == hi:
        return float(s[lo])
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def build_pressure_points(
    per_run_rows: List[Dict],
    *,
    deadline_ps: int,
    dram_capacity_mb: float = 1024.0,
    sim_ns_per_event: float = 100.0,
    line_bits: int = 512,
    deadline_ms: float = 33.0,
    group_extra: Optional[Tuple[str, ...]] = None,
) -> List[Dict]:
    """Pool per_run_summary rows the same way analyze_ecc_results.py does.

    group_extra: additional key columns appended to the grouping tuple
    (e.g. ('target_kernel', 'campaign_mode') for campaign merges).
    """
    extra = group_extra or ()
    by_config: Dict[Tuple, List[Dict]] = defaultdict(list)
    for r in per_run_rows:
        if r.get("end_to_end_ps", "") == "":
            continue
        key = (r["scheme"], r["policy"], r["ber"],
               r["fault_model"], r["due_action"])
        for col in extra:
            key = key + (r.get(col, ""),)
        by_config[key].append(r)

    pp_rows: List[Dict] = []
    for key, rows in sorted(by_config.items()):
        scheme, policy, ber, fm, due = key[:5]
        e2es = [int(r["end_to_end_ps"]) for r in rows if r.get("end_to_end_ps", "") != ""]
        latencies = [int(r["ecc_latency_ps"]) for r in rows if r.get("ecc_latency_ps", "") != ""]
        if not e2es:
            continue

        events_total_sum = sum(_as_int(r.get("events_total")) for r in rows)
        events_escape_sum = sum(_as_int(r.get("events_escape")) for r in rows)
        frames_total_sum = sum(_as_int(r.get("frames_total")) for r in rows)
        frames_unsafe_sum = sum(_as_int(r.get("frames_unsafe")) for r in rows)
        frames_dropped_sum = sum(_as_int(r.get("frames_dropped")) for r in rows)
        frames_argmax_sum = sum(_as_int(r.get("frames_argmax_diff")) for r in rows)
        viol_count = sum(1 for v in e2es if v > deadline_ps)
        n_seeds = len(e2es)

        viol_p, viol_lo, viol_hi = wilson_ci(viol_count, n_seeds)
        sdc_p, sdc_lo, sdc_hi = wilson_ci(events_escape_sum, events_total_sum)
        unsafe_p, unsafe_lo, unsafe_hi = wilson_ci(frames_unsafe_sum, frames_total_sum)
        drop_p, drop_lo, drop_hi = wilson_ci(frames_dropped_sum, frames_total_sum)
        argmax_p, argmax_lo, argmax_hi = wilson_ci(frames_argmax_sum, frames_total_sum)

        try:
            ber_f = float(ber)
        except (TypeError, ValueError):
            ber_f = 0.0
        fit_equiv = ""
        if ber_f > 0.0 and dram_capacity_mb > 0:
            line_event_rate = line_bits * ber_f
            events_per_hour = 3.6e12 / sim_ns_per_event
            fit_equiv_val = (line_event_rate * events_per_hour) / (1e-9 * dram_capacity_mb)
            fit_equiv = "{:.3e}".format(fit_equiv_val)

        def _f(v: float, fmt: str = "{:.3e}") -> str:
            return "" if math.isnan(v) else fmt.format(v)

        row = {
            "scheme": scheme,
            "policy": policy,
            "ber": ber,
            "fit_per_mbit_per_hour_equiv": fit_equiv,
            "fault_model": fm,
            "due_action": due,
            "n_seeds": n_seeds,
            "events_total": events_total_sum,
            "frames_total": frames_total_sum,
            "p50_e2e_ps": int(percentile(e2es, 50)),
            "p95_e2e_ps": int(percentile(e2es, 95)),
            "mean_ecc_latency_ps": int(sum(latencies) / max(len(latencies), 1)) if latencies else 0,
            "deadline_viol_rate": _f(viol_p, "{:.4f}"),
            "deadline_viol_lo": _f(viol_lo, "{:.4f}"),
            "deadline_viol_hi": _f(viol_hi, "{:.4f}"),
            "sdc_rate": _f(sdc_p),
            "sdc_rate_lo": _f(sdc_lo),
            "sdc_rate_hi": _f(sdc_hi),
            "unsafe_action_rate": _f(unsafe_p),
            "unsafe_action_lo": _f(unsafe_lo),
            "unsafe_action_hi": _f(unsafe_hi),
            "drop_rate": _f(drop_p),
            "drop_rate_lo": _f(drop_lo),
            "drop_rate_hi": _f(drop_hi),
            "argmax_change_rate": _f(argmax_p),
            "argmax_change_lo": _f(argmax_lo),
            "argmax_change_hi": _f(argmax_hi),
            "deadline_ms": deadline_ms,
        }
        if extra:
            for i, col in enumerate(extra):
                row[col] = key[5 + i]
        pp_rows.append(row)
    return pp_rows


def apply_canonical_slice(
    pp_rows: List[Dict], canonical_slice: str
) -> Tuple[List[Dict], List[Dict], Optional[str], Optional[str]]:
    """Split pressure_points into (full, canonical) like the analyzer."""
    if not canonical_slice or "+" not in canonical_slice:
        return pp_rows, pp_rows, None, None
    fm, due = canonical_slice.split("+", 1)
    canonical = [r for r in pp_rows
                 if r.get("fault_model") == fm and r.get("due_action") == due]
    return pp_rows, canonical, fm, due


def merge_raw_run_dirs(
    shard_dirs: Sequence[str],
    merged_dir: str,
    *,
    index_key_cols: Sequence[str],
) -> Tuple[int, int, List[str]]:
    """Merge index.csv, logs, and goldens from shard run dirs into merged_dir.

    Returns (n_index_rows, n_logs_linked, warnings).
    """
    os.makedirs(merged_dir, exist_ok=True)
    goldens_merged = os.path.join(merged_dir, "goldens")
    os.makedirs(goldens_merged, exist_ok=True)

    warnings: List[str] = []
    index_rows: List[Dict[str, str]] = []
    seen_index: set = set()

    for shard_dir in shard_dirs:
        idx_path = os.path.join(shard_dir, "index.csv")
        for row in read_csv(idx_path):
            key = tuple(row.get(c, "") for c in index_key_cols)
            if key in seen_index:
                warnings.append(
                    f"duplicate index row {key} in {idx_path}; keeping first")
                continue
            seen_index.add(key)
            index_rows.append(row)

            log_name = row.get("log", "")
            if not log_name:
                continue
            src_log = os.path.join(shard_dir, log_name)
            dst_log = os.path.join(merged_dir, log_name)
            if not os.path.isfile(src_log):
                warnings.append(f"missing log {src_log}")
                continue
            if os.path.lexists(dst_log):
                continue
            os.symlink(os.path.relpath(src_log, os.path.dirname(dst_log)), dst_log)

        shard_goldens = os.path.join(shard_dir, "goldens")
        if os.path.isdir(shard_goldens):
            for name in os.listdir(shard_goldens):
                src = os.path.join(shard_goldens, name)
                dst = os.path.join(goldens_merged, name)
                if name.endswith(".csv") and not os.path.lexists(dst):
                    shutil.copy2(src, dst)

    if index_rows:
        write_csv(os.path.join(merged_dir, "index.csv"), index_rows)

    n_logs = len([f for f in os.listdir(merged_dir) if f.endswith(".log")])
    return len(index_rows), n_logs, warnings


def merge_analysis_from_shards(
    shard_analysis_dirs: Sequence[str],
    merged_analysis_dir: str,
    *,
    deadline_ms: float = 33.0,
    dram_capacity_mb: float = 1024.0,
    sim_ns_per_event: float = 100.0,
    line_bits: int = 512,
    canonical_slice: str = "",
    group_extra: Optional[Tuple[str, ...]] = None,
) -> None:
    """Concatenate per-shard analysis CSVs and rebuild pressure_points."""
    os.makedirs(merged_analysis_dir, exist_ok=True)
    tables = (
        "per_run_summary.csv",
        "per_kernel_overhead.csv",
        "per_region_overhead.csv",
        "per_frame_safety.csv",
        "fault_mode_mix.csv",
        "rejected_runs.csv",
    )
    for name in tables:
        paths = [os.path.join(d, name) for d in shard_analysis_dirs
                 if os.path.isfile(os.path.join(d, name))]
        rows = concat_csv_files(paths)
        if rows:
            write_csv(os.path.join(merged_analysis_dir, name), rows)

    per_run = read_csv(os.path.join(merged_analysis_dir, "per_run_summary.csv"))
    if not per_run:
        print("WARN: no per_run_summary rows to pool", file=sys.stderr)
        return

    deadline_ps = int(deadline_ms * 1e9)
    pp_full = build_pressure_points(
        per_run,
        deadline_ps=deadline_ps,
        dram_capacity_mb=dram_capacity_mb,
        sim_ns_per_event=sim_ns_per_event,
        line_bits=line_bits,
        deadline_ms=deadline_ms,
        group_extra=group_extra,
    )
    pp_all, pp_canonical, canonical_fm, _ = apply_canonical_slice(
        pp_full, canonical_slice)

    if pp_all:
        full_name = ("pressure_points_full.csv" if canonical_fm
                     else "pressure_points.csv")
        write_csv(os.path.join(merged_analysis_dir, full_name), pp_all)
    if canonical_fm and pp_canonical:
        write_csv(os.path.join(merged_analysis_dir, "pressure_points.csv"),
                  pp_canonical)


def run_analyzer(
    run_dir: str,
    analysis_dir: str,
    analyzer_script: str,
    extra_args: Sequence[str],
) -> int:
    cmd = [sys.executable, analyzer_script, run_dir, "--out", analysis_dir]
    cmd.extend(extra_args)
    print("Running:", " ".join(cmd))
    return subprocess.call(cmd)
