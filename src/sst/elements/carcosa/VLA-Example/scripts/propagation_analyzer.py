#!/usr/bin/env python3
"""Phase-1 fault-propagation analyzer.

Reads the per-run logs that `run_case_studies.sh` produced (index.csv +
case_*.log) and emits two CSVs into <run_dir>/analysis/:

  * propagation_trace.csv     - one row per (case, scheme, seed, pipeline_cycle)
                                with the fault-injection counters that landed
                                during the frame, the kernel that was running
                                when the dominant escape fired, the action vs
                                golden checksum, and the ActionScorer outcome
                                (O1/O2/O3/O4) that resulted. Lets you trace
                                "a fault landed here -> ended up over there"
                                without re-parsing logs by hand.
  * propagation_summary.csv   - one row per (case_id, scheme) aggregating the
                                trace into: events_total, events_correctable,
                                events_due, events_escape, ecc_latency_ps,
                                frames_total/O1/O2/O3/O4, fractions of all
                                three propagation channels (drop, checksum,
                                silent), mean per-frame latency, and the
                                addr-filter region that was targeted.

Both files are written even when there are zero faults (C0 baselines), so
plots and acceptance scripts always have a column to dereference.

Usage (from anywhere):
    python3 propagation_analyzer.py /path/to/case_studies
    python3 propagation_analyzer.py /path/to/case_studies --out /tmp/prop

Imports parse_run_log + index_runs from analyze_ecc_results.py so the parser
contract stays in one place.
"""
from __future__ import annotations

import argparse
import csv
import os
import sys
from collections import defaultdict
from typing import Any, Dict, List, Optional, Tuple

# Reuse the case-study log parser + index reader. analyze_ecc_results.py is in
# the same directory, so make sure that directory is on sys.path before the
# import (covers both `python3 propagation_analyzer.py` and module-style use).
HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
from analyze_ecc_results import (  # noqa: E402
    parse_run_log,
    index_runs,
)


# ---------------------------------------------------------------------------
# Per-run derived metrics
# ---------------------------------------------------------------------------

# ActionScorer outcome classes (see ActionScorer.cc::classifyFrame):
#   O1: clean (no escape, no checksum change, no drop)
#   O2: DUE drop_frame (frame aborted by EccGuard; checksum may be stale)
#   O3: silent escape that reached the action checksum (unsafe_action)
#   O4: silent escape that did NOT change the checksum -> covered SDC
_OUTCOME_LABELS = {
    "O1": "clean",
    "O2": "drop_frame",
    "O3": "checksum_changed",
    "O4": "covered_sdc",
}


def _ecc_totals(parsed: Dict[str, Any]) -> Dict[str, int]:
    """Sum the EccGuard per-kernel block into a single counter set."""
    ecc = parsed.get("ecc") or []
    totals = {
        "events_clean":       0,
        "events_correctable": 0,
        "events_due":         0,
        "events_escape":      0,
        "ecc_latency_ps":     0,
    }
    for r in ecc:
        totals["events_clean"]       += int(r.get("clean", 0) or 0)
        totals["events_correctable"] += int(r.get("correctable", 0) or 0)
        totals["events_due"]         += int(r.get("due", 0) or 0)
        totals["events_escape"]      += int(r.get("escape", 0) or 0)
        totals["ecc_latency_ps"]     += int(r.get("latency_ps", 0) or 0)
    totals["events_total"] = (totals["events_clean"]
                              + totals["events_correctable"]
                              + totals["events_due"]
                              + totals["events_escape"])
    return totals


def _kernel_with_max_events(parsed: Dict[str, Any], key: str) -> str:
    """Name of the kernel with the most events of `key` ('escape' / 'due' / ...)."""
    ecc = parsed.get("ecc") or []
    best_name = ""
    best_count = -1
    for r in ecc:
        n = int(r.get(key, 0) or 0)
        if n > best_count:
            best_count = n
            best_name = str(r.get("kernel_name", "") or "")
    return best_name if best_count > 0 else ""


def _region_with_max_events(parsed: Dict[str, Any], key: str) -> str:
    """Name of the region (across all kernels) with the most events of `key`."""
    rows = parsed.get("region") or []
    agg: Dict[str, int] = defaultdict(int)
    for r in rows:
        agg[str(r.get("region", "") or "")] += int(r.get(key, 0) or 0)
    if not agg:
        return ""
    name, count = max(agg.items(), key=lambda kv: kv[1])
    return name if count > 0 else ""


def _path_label(fr: Dict[str, Any]) -> str:
    """Human-readable propagation channel for one scored frame."""
    if int(fr.get("dropped", 0) or 0):
        return "drop_frame"
    if int(fr.get("argmax_changed", 0) or 0):
        return "checksum_changed"
    if int(fr.get("escapes_in_frame", 0) or 0):
        return "covered_sdc"
    return "clean"


# ---------------------------------------------------------------------------
# Trace + summary builders
# ---------------------------------------------------------------------------


def build_trace(runs: List[Dict[str, Any]]) -> Tuple[List[Dict[str, Any]],
                                                     Dict[str, Dict[str, Any]]]:
    """Return (trace_rows, parsed_by_log) for all runs that exited cleanly."""
    parsed_by_log: Dict[str, Dict[str, Any]] = {}
    trace_rows: List[Dict[str, Any]] = []

    for run in runs:
        if int(run.get("exit", 1) or 1) != 0:
            continue
        log_path = os.path.realpath(run["log"])
        parsed = parsed_by_log.get(log_path) or parse_run_log(log_path)
        parsed_by_log[log_path] = parsed

        ecc_totals = _ecc_totals(parsed)
        dominant_escape_kernel = _kernel_with_max_events(parsed, "escape")
        dominant_escape_region = _region_with_max_events(parsed, "escape")
        dominant_due_kernel    = _kernel_with_max_events(parsed, "due")

        frames = parsed.get("frames") or []
        if not frames:
            # C0 / baseline can legitimately produce zero frames if SST
            # killed early. Emit a sentinel row so the summary still has a
            # bucket for this run instead of silently dropping it.
            trace_rows.append({
                "case_id":               run.get("case_id", ""),
                "scheme":                run.get("scheme", ""),
                "policy":                run.get("policy", ""),
                "latency_profile":       run.get("latency_profile", "default"),
                "seed":                  run.get("seed", ""),
                "pipeline_cycle":        0,
                "kernel_at_close":       "",
                "attributing_kernel":    dominant_escape_kernel or "",
                "dominant_escape_region": dominant_escape_region,
                "escapes_in_frame":      0,
                "flips_in_frame":        0,
                "action_checksum":       0,
                "golden_checksum":       0,
                "checksum_delta_hex":    "0x0",
                "dropped":               0,
                "argmax_changed":        0,
                "safety_violated":       0,
                "outcome_class":         "",
                "propagation_path":      "no_frames",
                "events_total_run":      ecc_totals["events_total"],
                "events_escape_run":     ecc_totals["events_escape"],
                "events_due_run":        ecc_totals["events_due"],
                "sim_time_ps":           0,
            })
            continue

        for fr in frames:
            ac = int(fr.get("action_checksum", 0) or 0)
            gc = int(fr.get("golden_checksum", 0) or 0)
            attr = fr.get("attributing_kernel_name", "") or \
                   fr.get("kernel_name", "")
            trace_rows.append({
                "case_id":               run.get("case_id", ""),
                "scheme":                run.get("scheme", ""),
                "policy":                run.get("policy", ""),
                "latency_profile":       run.get("latency_profile", "default"),
                "seed":                  run.get("seed", ""),
                "pipeline_cycle":        int(fr.get("pipeline_cycle", 0) or 0),
                "kernel_at_close":       fr.get("kernel_name", "") or "",
                "attributing_kernel":    attr,
                # Run-level (not per-frame) hint about which region took the
                # most escapes overall: per-frame per-region attribution
                # isn't published by EccGuard yet, so we provide it once as
                # context. If multiple frames had escapes, the dominant
                # region is the same across rows for that run.
                "dominant_escape_region": dominant_escape_region,
                "escapes_in_frame":      int(fr.get("escapes_in_frame", 0) or 0),
                "flips_in_frame":        int(fr.get("flips_in_frame", 0) or 0),
                "action_checksum":       ac,
                "golden_checksum":       gc,
                "checksum_delta_hex":    "0x%x" % (ac ^ gc),
                "dropped":               int(fr.get("dropped", 0) or 0),
                "argmax_changed":        int(fr.get("argmax_changed", 0) or 0),
                "safety_violated":       int(fr.get("safety_violated", 0) or 0),
                "outcome_class":         fr.get("outcome_class", "") or "",
                "propagation_path":      _path_label(fr),
                # Carry the run-level escape/due counters so a single row
                # tells the whole "fault landed here -> ended up over there"
                # story without joining with the per-run summary.
                "events_total_run":      ecc_totals["events_total"],
                "events_escape_run":     ecc_totals["events_escape"],
                "events_due_run":        ecc_totals["events_due"],
                "sim_time_ps":           int(fr.get("sim_time_ps", 0) or 0),
            })

    return trace_rows, parsed_by_log


def build_summary(runs: List[Dict[str, Any]],
                  parsed_by_log: Dict[str, Dict[str, Any]]) -> List[Dict[str, Any]]:
    """Aggregate per (case_id, scheme, latency_profile)."""
    by_key: Dict[Tuple[str, str, str], List[Dict[str, Any]]] = defaultdict(list)
    for run in runs:
        if int(run.get("exit", 1) or 1) != 0:
            continue
        key = (run.get("case_id", ""),
               run.get("scheme", ""),
               run.get("latency_profile", "default"))
        by_key[key].append(run)

    rows: List[Dict[str, Any]] = []
    for (case_id, scheme, lat), group in sorted(by_key.items()):
        agg = {
            "case_id":               case_id,
            "scheme":                scheme,
            "latency_profile":       lat,
            "runs":                  0,
            "events_total":          0,
            "events_clean":          0,
            "events_correctable":    0,
            "events_due":            0,
            "events_escape":         0,
            "ecc_latency_ps":        0,
            "frames_total":          0,
            "frames_outcome_O1":     0,
            "frames_outcome_O2":     0,
            "frames_outcome_O3":     0,
            "frames_outcome_O4":     0,
            "frames_dropped":        0,
            "frames_argmax_changed": 0,
            "sim_time_ps_max":       0,
        }
        for run in group:
            parsed = parsed_by_log.get(os.path.realpath(run["log"]), {})
            tot = _ecc_totals(parsed)
            agg["runs"]                 += 1
            agg["events_total"]         += tot["events_total"]
            agg["events_clean"]         += tot["events_clean"]
            agg["events_correctable"]   += tot["events_correctable"]
            agg["events_due"]           += tot["events_due"]
            agg["events_escape"]        += tot["events_escape"]
            agg["ecc_latency_ps"]       += tot["ecc_latency_ps"]
            scorer = parsed.get("scorer") or {}
            agg["frames_total"]         += int(scorer.get("frames_total", 0) or 0)
            agg["frames_outcome_O1"]    += int(scorer.get("frames_outcome_O1", 0) or 0)
            agg["frames_outcome_O2"]    += int(scorer.get("frames_outcome_O2", 0) or 0)
            agg["frames_outcome_O3"]    += int(scorer.get("frames_outcome_O3", 0) or 0)
            agg["frames_outcome_O4"]    += int(scorer.get("frames_outcome_O4", 0) or 0)
            agg["frames_dropped"]       += int(scorer.get("frames_dropped", 0) or 0)
            agg["frames_argmax_changed"]+= int(scorer.get("frames_argmax_diff", 0) or 0)
            for fr in parsed.get("frames", []):
                t = int(fr.get("sim_time_ps", 0) or 0)
                if t > agg["sim_time_ps_max"]:
                    agg["sim_time_ps_max"] = t

        ft = agg["frames_total"] or 1
        # Propagation fractions: of the frames that experienced at least one
        # ECC event (O2/O3/O4), which channel did it end up in?
        injected = (agg["frames_outcome_O2"]
                    + agg["frames_outcome_O3"]
                    + agg["frames_outcome_O4"])
        inj_div = injected or 1
        agg["fraction_O1"] = agg["frames_outcome_O1"] / ft
        agg["fraction_O2"] = agg["frames_outcome_O2"] / ft
        agg["fraction_O3"] = agg["frames_outcome_O3"] / ft
        agg["fraction_O4"] = agg["frames_outcome_O4"] / ft
        agg["propagation_drop_rate"]     = agg["frames_outcome_O2"] / inj_div
        agg["propagation_checksum_rate"] = agg["frames_outcome_O3"] / inj_div
        agg["propagation_covered_rate"]  = agg["frames_outcome_O4"] / inj_div
        agg["mean_latency_ps_per_frame"] = (
            agg["ecc_latency_ps"] / ft) if ft else 0.0
        rows.append(agg)
    return rows


# ---------------------------------------------------------------------------
# I/O helpers
# ---------------------------------------------------------------------------


def _write_csv(path: str, rows: List[Dict[str, Any]]) -> None:
    if not rows:
        # Always create the file with at least a header so plotting / preflight
        # scripts can open it unconditionally.
        with open(path, "w", newline="") as f:
            f.write("# no runs to summarize\n")
        print(f"Wrote {path} (empty)")
        return
    fields: List[str] = []
    seen: Dict[str, None] = {}
    for r in rows:
        for k in r.keys():
            if k not in seen:
                seen[k] = None
                fields.append(k)
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)
    print(f"Wrote {path}")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("run_dir", help="case_studies directory (contains index.csv).")
    ap.add_argument("--out", default=None,
                    help="Output directory (default: <run_dir>/analysis).")
    args = ap.parse_args()

    out_dir = args.out or os.path.join(args.run_dir, "analysis")
    os.makedirs(out_dir, exist_ok=True)

    runs = index_runs(out_dir, args.run_dir)
    if not runs:
        print(f"ERROR: no runs indexed under {args.run_dir}", file=sys.stderr)
        return 2

    trace_rows, parsed_by_log = build_trace(runs)
    summary_rows               = build_summary(runs, parsed_by_log)

    _write_csv(os.path.join(out_dir, "propagation_trace.csv"),   trace_rows)
    _write_csv(os.path.join(out_dir, "propagation_summary.csv"), summary_rows)

    # Print a one-line headline so callers see something useful even without
    # opening the CSVs.
    if summary_rows:
        print()
        print("=== Propagation Summary ===")
        print(f"{'case':>5} {'scheme':>9} {'runs':>4} {'frames':>6} "
              f"{'O1':>4} {'O2':>4} {'O3':>4} {'O4':>4} "
              f"{'drop_path':>10} {'cs_path':>9} {'covered':>9}")
        for r in summary_rows:
            inj = r["frames_outcome_O2"] + r["frames_outcome_O3"] + r["frames_outcome_O4"]
            print(f"{r['case_id']:>5} {r['scheme']:>9} {r['runs']:>4} "
                  f"{r['frames_total']:>6} "
                  f"{r['frames_outcome_O1']:>4} {r['frames_outcome_O2']:>4} "
                  f"{r['frames_outcome_O3']:>4} {r['frames_outcome_O4']:>4} "
                  f"{r['propagation_drop_rate']:>10.3f} "
                  f"{r['propagation_checksum_rate']:>9.3f} "
                  f"{r['propagation_covered_rate']:>9.3f} "
                  f"(inj_frames={inj})")
        print("=== End Propagation Summary ===")

    return 0


if __name__ == "__main__":
    sys.exit(main())
