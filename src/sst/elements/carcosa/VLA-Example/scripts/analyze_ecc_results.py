#!/usr/bin/env python3
"""Aggregate ECC sweep / spot-check logs into per-run, per-kernel, and
pressure-point CSVs plus a summary.txt. See run_ecc_sweep.sh."""

from __future__ import annotations

import argparse
import csv
import math
import os
import re
import sys
from collections import defaultdict
from typing import Dict, Iterable, List, Optional, Tuple

ECC_BLOCK_START = re.compile(r"=== EccGuard .* Per-Kernel Outcomes ===")
ECC_BLOCK_END = re.compile(r"=== End EccGuard .* Per-Kernel Outcomes ===")
ECC_HEADER_LINE = "kernel_id,kernel_name,clean,correctable,due,escape,latency_ps"

VLA_BLOCK_START = re.compile(r"=== VLA Per-Kernel Profile.*===")
VLA_BLOCK_END = re.compile(r"=== End .*Profile.*===")
VLA_HEADER_RE = re.compile(r"^core,kernel_id,kernel_name,start_[a-z]+,end_[a-z]+,delta_[a-z]+\s*$")


def parse_run_log(path: str) -> Dict:
    with open(path, "r") as f:
        lines = f.read().splitlines()

    ecc_rows: List[Dict] = []
    vla_rows: List[Dict] = []

    in_ecc = False
    saw_header = False
    for line in lines:
        if ECC_BLOCK_START.search(line):
            in_ecc = True
            saw_header = False
            continue
        if ECC_BLOCK_END.search(line):
            in_ecc = False
            continue
        if in_ecc:
            line = line.strip()
            if not line:
                continue
            if not saw_header:
                if line == ECC_HEADER_LINE:
                    saw_header = True
                continue
            parts = line.split(",")
            if len(parts) != 7:
                continue
            try:
                ecc_rows.append({
                    "kernel_id":   int(parts[0]),
                    "kernel_name": parts[1],
                    "clean":       int(parts[2]),
                    "correctable": int(parts[3]),
                    "due":         int(parts[4]),
                    "escape":      int(parts[5]),
                    "latency_ps":  int(parts[6]),
                })
            except ValueError:
                continue

    in_vla = False
    saw_vla_header = False
    for line in lines:
        if VLA_BLOCK_START.search(line):
            in_vla = True
            saw_vla_header = False
            continue
        if VLA_BLOCK_END.search(line):
            in_vla = False
            continue
        if in_vla:
            stripped = line.strip()
            if not stripped:
                continue
            if not saw_vla_header:
                if VLA_HEADER_RE.match(stripped):
                    saw_vla_header = True
                continue
            parts = stripped.split(",")
            if len(parts) != 6:
                continue
            try:
                vla_rows.append({
                    "core":        parts[0],
                    "kernel_id":   int(parts[1]),
                    "kernel_name": parts[2],
                    "start":       int(parts[3]),
                    "end":         int(parts[4]),
                    "delta":       int(parts[5]),
                })
            except ValueError:
                continue

    return {"ecc": ecc_rows, "vla": vla_rows}


def end_to_end_ps(vla_rows: List[Dict]) -> Optional[int]:
    if not vla_rows:
        return None
    starts = [r["start"] for r in vla_rows]
    ends   = [r["end"]   for r in vla_rows]
    return max(ends) - min(starts)


def index_runs(out_dir: str, run_dir: str) -> List[Dict]:
    runs: List[Dict] = []
    idx = os.path.join(run_dir, "index.csv")
    if os.path.exists(idx):
        with open(idx, "r") as f:
            reader = csv.DictReader(f)
            for row in reader:
                runs.append({
                    "ber":    row.get("ber", "0"),
                    "scheme": row.get("scheme", "none"),
                    "policy": row.get("policy", "uniform"),
                    "seed":   row.get("seed", "0"),
                    "log":    os.path.join(run_dir, row.get("log", "")),
                    "exit":   int(row.get("exit", 0) or 0),
                })
    else:
        for name in sorted(os.listdir(run_dir)):
            if not name.endswith(".log"):
                continue
            runs.append({
                "ber":    "?",
                "scheme": "?",
                "policy": "?",
                "seed":   "?",
                "log":    os.path.join(run_dir, name),
                "exit":   0,
            })
    return runs


def percentile(vals: List[float], p: float) -> float:
    if not vals:
        return float("nan")
    s = sorted(vals)
    if len(s) == 1:
        return s[0]
    k = (len(s) - 1) * (p / 100.0)
    lo = int(math.floor(k))
    hi = int(math.ceil(k))
    if lo == hi:
        return s[lo]
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("run_dir", help="Directory containing per-run logs (and index.csv).")
    ap.add_argument("--out", default=None, help="Output directory (default: <run_dir>/analysis).")
    ap.add_argument("--deadline-ms", type=float, default=33.0,
                    help="Actuation deadline in ms; latencies above this count as violations.")
    ap.add_argument("--sdc-budget", type=float, default=1e-6,
                    help="Per-actuation SDC rate budget for the pressure-point line.")
    args = ap.parse_args()

    if not os.path.isdir(args.run_dir):
        print(f"ERROR: run_dir '{args.run_dir}' not found.", file=sys.stderr)
        return 2

    out_dir = args.out or os.path.join(args.run_dir, "analysis")
    os.makedirs(out_dir, exist_ok=True)

    runs = index_runs(out_dir, args.run_dir)
    if not runs:
        print("ERROR: no run logs found.", file=sys.stderr)
        return 2

    deadline_ps = int(args.deadline_ms * 1e9)

    per_run_rows = []
    per_kernel_rows = []
    for run in runs:
        if run["exit"] != 0 or not os.path.exists(run["log"]):
            continue
        parsed = parse_run_log(run["log"])
        e2e = end_to_end_ps(parsed["vla"])

        ecc_total_latency = sum(r["latency_ps"] for r in parsed["ecc"])
        ecc_escape_total  = sum(r["escape"]     for r in parsed["ecc"])
        ecc_due_total     = sum(r["due"]        for r in parsed["ecc"])
        ecc_correct_total = sum(r["correctable"] for r in parsed["ecc"])
        ecc_clean_total   = sum(r["clean"]      for r in parsed["ecc"])
        events_total      = ecc_total_latency_count = (
            ecc_clean_total + ecc_correct_total + ecc_due_total + ecc_escape_total
        )

        per_run_rows.append({
            "ber":               run["ber"],
            "scheme":            run["scheme"],
            "policy":            run["policy"],
            "seed":              run["seed"],
            "end_to_end_ps":     e2e if e2e is not None else "",
            "deadline_violated": (1 if (e2e is not None and e2e > deadline_ps) else 0),
            "ecc_latency_ps":    ecc_total_latency,
            "events_total":      events_total,
            "events_clean":      ecc_clean_total,
            "events_correctable": ecc_correct_total,
            "events_due":        ecc_due_total,
            "events_escape":     ecc_escape_total,
        })

        for r in parsed["ecc"]:
            per_kernel_rows.append({
                "ber":         run["ber"],
                "scheme":      run["scheme"],
                "policy":      run["policy"],
                "seed":        run["seed"],
                "kernel_id":   r["kernel_id"],
                "kernel_name": r["kernel_name"],
                "clean":       r["clean"],
                "correctable": r["correctable"],
                "due":         r["due"],
                "escape":      r["escape"],
                "latency_ps":  r["latency_ps"],
            })

    if per_run_rows:
        with open(os.path.join(out_dir, "per_run_summary.csv"), "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(per_run_rows[0].keys()))
            writer.writeheader()
            writer.writerows(per_run_rows)

    if per_kernel_rows:
        with open(os.path.join(out_dir, "per_kernel_overhead.csv"), "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(per_kernel_rows[0].keys()))
            writer.writeheader()
            writer.writerows(per_kernel_rows)

    # Pressure-point curve: P50/P95, mean correction cost, deadline-violation rate, SDC rate.
    by_config: Dict[Tuple[str, str, str], List[Dict]] = defaultdict(list)
    for r in per_run_rows:
        if r["end_to_end_ps"] == "":
            continue
        by_config[(r["scheme"], r["policy"], r["ber"])].append(r)

    pp_rows = []
    for (scheme, policy, ber), rows in sorted(by_config.items()):
        e2es = [int(r["end_to_end_ps"]) for r in rows if r["end_to_end_ps"] != ""]
        latencies = [int(r["ecc_latency_ps"]) for r in rows]
        sdc_counts = [int(r["events_escape"]) for r in rows]
        events = [max(int(r["events_total"]), 1) for r in rows]
        if not e2es:
            continue
        viol = sum(1 for v in e2es if v > deadline_ps) / len(e2es)
        sdc_rate = sum(sdc_counts) / max(sum(events), 1)
        pp_rows.append({
            "scheme":             scheme,
            "policy":             policy,
            "ber":                ber,
            "n_seeds":            len(e2es),
            "p50_e2e_ps":         int(percentile(e2es, 50)),
            "p95_e2e_ps":         int(percentile(e2es, 95)),
            "mean_ecc_latency_ps": int(sum(latencies) / max(len(latencies), 1)),
            "deadline_viol_rate": f"{viol:.4f}",
            "sdc_rate":           f"{sdc_rate:.3e}",
            "deadline_ms":        args.deadline_ms,
        })

    if pp_rows:
        with open(os.path.join(out_dir, "pressure_points.csv"), "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(pp_rows[0].keys()))
            writer.writeheader()
            writer.writerows(pp_rows)

    summary_path = os.path.join(out_dir, "summary.txt")
    with open(summary_path, "w") as f:
        f.write("ECC pressure-point analysis\n")
        f.write("===========================\n")
        f.write(f"run_dir         : {os.path.abspath(args.run_dir)}\n")
        f.write(f"runs ingested   : {len(per_run_rows)}\n")
        f.write(f"deadline (ms)   : {args.deadline_ms}\n")
        f.write(f"sdc_budget      : {args.sdc_budget}\n\n")

        f.write("Pressure-point table:\n")
        f.write("scheme  policy        ber          P50_e2e_ms  P95_e2e_ms  mean_ecc_us  viol_rate  sdc_rate\n")
        for r in pp_rows:
            f.write("{:<7} {:<13} {:<12} {:<11.3f} {:<11.3f} {:<12.3f} {:<10} {}\n".format(
                r["scheme"], r["policy"], r["ber"],
                r["p50_e2e_ps"] / 1e9, r["p95_e2e_ps"] / 1e9,
                r["mean_ecc_latency_ps"] / 1e6,
                r["deadline_viol_rate"], r["sdc_rate"]))
        f.write("\n")

        # Smallest BER per (scheme, policy) where deadline-violation >= 5% OR sdc exceeds budget.
        f.write("Pressure points (first BER violating either deadline >5% or sdc_budget):\n")
        groups: Dict[Tuple[str, str], List[Dict]] = defaultdict(list)
        for r in pp_rows:
            groups[(r["scheme"], r["policy"])].append(r)
        for (scheme, policy), rows in groups.items():
            try:
                rows_sorted = sorted(rows, key=lambda r: float(r["ber"]))
            except ValueError:
                rows_sorted = rows
            crossover = None
            for r in rows_sorted:
                viol = float(r["deadline_viol_rate"])
                sdc  = float(r["sdc_rate"])
                if viol >= 0.05 or sdc >= args.sdc_budget:
                    crossover = r["ber"]
                    break
            f.write(f"  scheme={scheme:<8} policy={policy:<13} pressure_point_ber={crossover}\n")

    print(f"Wrote {summary_path}")
    print(f"Wrote {os.path.join(out_dir, 'per_run_summary.csv')}")
    print(f"Wrote {os.path.join(out_dir, 'per_kernel_overhead.csv')}")
    print(f"Wrote {os.path.join(out_dir, 'pressure_points.csv')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
