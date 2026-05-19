#!/usr/bin/env python3
"""Aggregate ECC sweep / spot-check logs into per-run, per-kernel, per-region,
per-frame, and pressure-point CSVs plus a summary.txt. See run_ecc_sweep.sh.

Phase 5 additions:
  - Parse '=== EccGuard ... Per-Kernel-Per-Region Outcomes ===' blocks.
  - Parse '=== EccGuard ... Fault-Mode Draws ===' blocks.
  - Parse '=== Action Scorer ... Per-Frame Trace ===' blocks.
  - Parse '=== Action Scorer ... Summary ===' lines.
  - Parse '=== VLA Frame Drops ... ===' lines.
  - Emit per_region_overhead.csv, fault_mode_mix.csv, per_frame_safety.csv,
    plus a publication-grade pressure_points.csv that reports
    unsafe_action_rate, drop_rate, deadline_viol_rate, mean_ecc_latency_us.
"""

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
ECC_BLOCK_END   = re.compile(r"=== End EccGuard .* Per-Kernel Outcomes ===")
ECC_HEADER_LINE = "kernel_id,kernel_name,clean,correctable,due,escape,latency_ps"

ECC_REGION_BLOCK_START = re.compile(r"=== EccGuard .* Per-Kernel-Per-Region Outcomes ===")
ECC_REGION_BLOCK_END   = re.compile(r"=== End EccGuard .* Per-Kernel-Per-Region Outcomes ===")
ECC_REGION_HEADER_LINE = "kernel_id,kernel_name,region,clean,correctable,due,escape,latency_ps"

FAULT_MODE_BLOCK_START = re.compile(r"=== EccGuard .* Fault-Mode Draws ===")
FAULT_MODE_BLOCK_END   = re.compile(r"=== End EccGuard .* Fault-Mode Draws ===")
FAULT_MODE_HEADER_LINE = "mode,count"

ESCAPE_SUMMARY_BLOCK_START = re.compile(r"=== EccGuard .* Escape/Abort Summary ===")
ESCAPE_SUMMARY_BLOCK_END   = re.compile(r"=== End EccGuard .* Escape/Abort Summary ===")
ESCAPE_SUMMARY_HEADER      = "escape_high_blast,escape_low_blast,frames_aborted,payload_dtype"

VLA_BLOCK_START = re.compile(r"=== VLA Per-Kernel Profile.*===")
VLA_BLOCK_END   = re.compile(r"=== End .*Profile.*===")
VLA_HEADER_RE   = re.compile(r"^core,kernel_id,kernel_name,start_[a-z]+,end_[a-z]+,delta_[a-z]+\s*$")

FRAME_DROPS_BLOCK_START = re.compile(r"=== VLA Frame Drops .*===")
FRAME_DROPS_BLOCK_END   = re.compile(r"=== End VLA Frame Drops ===")

SCORER_TRACE_BLOCK_START = re.compile(r"=== Action Scorer .* Per-Frame Trace ===")
SCORER_TRACE_BLOCK_END   = re.compile(r"=== End Action Scorer .* Per-Frame Trace.*===")
# Two header layouts are supported: the legacy (pre-Tier B) layout has 11
# columns and is what every artifact prior to the metric-integrity-gate
# rebuild emits; the new layout adds attributing_kernel_id /
# attributing_kernel_name between kernel_name and dropped (13 columns).
# We accept either, fill defaults for the missing columns, and let
# downstream code key off the presence of attributing_kernel_name to
# decide whether Tier B Fig. 3a renders attribution-bars or fallback bars.
SCORER_TRACE_HEADER_LEGACY = ("pipeline_cycle,kernel_at_close,kernel_name,dropped,"
                              "escapes_in_frame,flips_in_frame,action_checksum,"
                              "golden_checksum,argmax_changed,safety_violated,sim_time_ps")
SCORER_TRACE_HEADER_TIERB  = ("pipeline_cycle,kernel_at_close,kernel_name,"
                              "attributing_kernel_id,attributing_kernel_name,dropped,"
                              "escapes_in_frame,flips_in_frame,action_checksum,"
                              "golden_checksum,argmax_changed,safety_violated,sim_time_ps")

def _scorer_header_match(line: str) -> bool:
    return line == SCORER_TRACE_HEADER_LEGACY or line == SCORER_TRACE_HEADER_TIERB

SCORER_SUMMARY_BLOCK_START = re.compile(r"=== Action Scorer .* Summary ===")
SCORER_SUMMARY_BLOCK_END   = re.compile(r"=== End Action Scorer .* Summary ===")
SCORER_SUMMARY_HEADER      = ("frames_total,frames_dropped,frames_argmax_diff,frames_unsafe,"
                              "drop_rate,argmax_change_rate,unsafe_action_rate")


def _scan_block(lines: List[str], start_re, end_re, header_match) -> List[List[str]]:
    """Generic block scanner: returns list of CSV-split rows from a block.
    header_match may be a string (exact match) or a callable(line)->bool."""
    out_rows = []
    in_block = False
    saw_header = False
    is_str_header = isinstance(header_match, str)
    for raw in lines:
        if start_re.search(raw):
            in_block = True
            saw_header = False
            continue
        if end_re.search(raw):
            in_block = False
            continue
        if not in_block:
            continue
        line = raw.strip()
        if not line:
            continue
        if not saw_header:
            ok = (line == header_match) if is_str_header else header_match(line)
            if ok:
                saw_header = True
            continue
        out_rows.append([c.strip() for c in line.split(",")])
    return out_rows


def parse_run_log(path: str) -> Dict:
    with open(path, "r") as f:
        lines = f.read().splitlines()

    ecc_rows: List[Dict] = []
    region_rows: List[Dict] = []
    vla_rows: List[Dict] = []
    fault_mode_rows: List[Dict] = []
    frame_rows: List[Dict] = []
    scorer_summary: Optional[Dict] = None
    escape_summary: Optional[Dict] = None
    frames_dropped_total: Optional[int] = None

    # Per-kernel outcomes (existing block).
    for parts in _scan_block(lines, ECC_BLOCK_START, ECC_BLOCK_END, ECC_HEADER_LINE):
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

    # Per-kernel-per-region outcomes (new in Phase 1).
    for parts in _scan_block(lines, ECC_REGION_BLOCK_START, ECC_REGION_BLOCK_END,
                             ECC_REGION_HEADER_LINE):
        if len(parts) != 8:
            continue
        try:
            region_rows.append({
                "kernel_id":   int(parts[0]),
                "kernel_name": parts[1],
                "region":      parts[2],
                "clean":       int(parts[3]),
                "correctable": int(parts[4]),
                "due":         int(parts[5]),
                "escape":      int(parts[6]),
                "latency_ps":  int(parts[7]),
            })
        except ValueError:
            continue

    # Fault-mode draws (new in Phase 2).
    for parts in _scan_block(lines, FAULT_MODE_BLOCK_START, FAULT_MODE_BLOCK_END,
                             FAULT_MODE_HEADER_LINE):
        if len(parts) != 2:
            continue
        try:
            fault_mode_rows.append({"mode": parts[0], "count": int(parts[1])})
        except ValueError:
            continue

    # Escape / abort summary (new in Phase 2/3).
    es_rows = _scan_block(lines, ESCAPE_SUMMARY_BLOCK_START, ESCAPE_SUMMARY_BLOCK_END,
                          ESCAPE_SUMMARY_HEADER)
    if es_rows:
        parts = es_rows[0]
        if len(parts) >= 4:
            try:
                escape_summary = {
                    "escape_high_blast": int(parts[0]),
                    "escape_low_blast":  int(parts[1]),
                    "frames_aborted":    int(parts[2]),
                    "payload_dtype":     parts[3],
                }
            except ValueError:
                escape_summary = None

    # VLA profile (existing block).
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

    # VLA frame drops summary (new in Phase 3).
    in_drops = False
    for raw in lines:
        if FRAME_DROPS_BLOCK_START.search(raw):
            in_drops = True
            continue
        if FRAME_DROPS_BLOCK_END.search(raw):
            in_drops = False
            continue
        if in_drops:
            line = raw.strip()
            if not line or line == "frames_dropped":
                continue
            try:
                frames_dropped_total = int(line)
                break
            except ValueError:
                pass

    # Action Scorer per-frame trace (Phase 4 + Tier B). Both 11-col
    # (legacy) and 13-col (Tier B with attributing_kernel) rows are
    # accepted; missing columns default to copies of kernel_at_close /
    # kernel_name so downstream consumers can treat the field as always
    # present.
    for parts in _scan_block(lines, SCORER_TRACE_BLOCK_START, SCORER_TRACE_BLOCK_END,
                             _scorer_header_match):
        try:
            if len(parts) == 13:
                frame_rows.append({
                    "pipeline_cycle":          int(parts[0]),
                    "kernel_at_close":         int(parts[1]),
                    "kernel_name":             parts[2],
                    "attributing_kernel_id":   int(parts[3]),
                    "attributing_kernel_name": parts[4],
                    "dropped":                 int(parts[5]),
                    "escapes_in_frame":        int(parts[6]),
                    "flips_in_frame":          int(parts[7]),
                    "action_checksum":         int(parts[8]),
                    "golden_checksum":         int(parts[9]),
                    "argmax_changed":          int(parts[10]),
                    "safety_violated":         int(parts[11]),
                    "sim_time_ps":             int(parts[12]),
                })
            elif len(parts) == 11:
                kac   = int(parts[1])
                kname = parts[2]
                frame_rows.append({
                    "pipeline_cycle":          int(parts[0]),
                    "kernel_at_close":         kac,
                    "kernel_name":             kname,
                    # Legacy artifact: no per-frame attribution available;
                    # fall back to kernel_at_close (always ACTUATE on the
                    # synthetic FSM). Tier B Fig. 3a will render a single-
                    # bar fallback chart with the appropriate caption.
                    "attributing_kernel_id":   kac,
                    "attributing_kernel_name": kname,
                    "dropped":                 int(parts[3]),
                    "escapes_in_frame":        int(parts[4]),
                    "flips_in_frame":          int(parts[5]),
                    "action_checksum":         int(parts[6]),
                    "golden_checksum":         int(parts[7]),
                    "argmax_changed":          int(parts[8]),
                    "safety_violated":         int(parts[9]),
                    "sim_time_ps":             int(parts[10]),
                })
        except ValueError:
            continue

    # Action Scorer summary line (Phase 4).
    ss_rows = _scan_block(lines, SCORER_SUMMARY_BLOCK_START, SCORER_SUMMARY_BLOCK_END,
                          SCORER_SUMMARY_HEADER)
    if ss_rows:
        parts = ss_rows[0]
        if len(parts) == 7:
            try:
                scorer_summary = {
                    "frames_total":         int(parts[0]),
                    "frames_dropped":       int(parts[1]),
                    "frames_argmax_diff":   int(parts[2]),
                    "frames_unsafe":        int(parts[3]),
                    "drop_rate":            float(parts[4]),
                    "argmax_change_rate":   float(parts[5]),
                    "unsafe_action_rate":   float(parts[6]),
                }
            except ValueError:
                scorer_summary = None

    return {
        "ecc":                  ecc_rows,
        "region":               region_rows,
        "vla":                  vla_rows,
        "fault_modes":          fault_mode_rows,
        "frames":               frame_rows,
        "scorer":               scorer_summary,
        "escape_summary":       escape_summary,
        "frames_dropped_total": frames_dropped_total,
    }


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
                    "ber":          row.get("ber", "0"),
                    "scheme":       row.get("scheme", "none"),
                    "policy":       row.get("policy", "uniform"),
                    "seed":         row.get("seed", "0"),
                    "fault_model":  row.get("fault_model", "poisson"),
                    "due_action":   row.get("due_action", "latency_only"),
                    "log":          os.path.join(run_dir, row.get("log", "")),
                    "exit":         int(row.get("exit", 0) or 0),
                    # Campaign-driver-only columns. They're absent in the
                    # standard sweep's index.csv, in which case .get returns
                    # "" and downstream consumers treat the row as a non-
                    # campaign cell. Keeping these inline (instead of in a
                    # nested dict) avoids a second parser path.
                    "target_kernel": row.get("target_kernel", ""),
                    "campaign_mode": row.get("campaign_mode", ""),
                    "event_budget":  row.get("event_budget",  ""),
                    "event_rate":    row.get("event_rate",    ""),
                })
    else:
        for name in sorted(os.listdir(run_dir)):
            if not name.endswith(".log"):
                continue
            runs.append({
                "ber":         "?",
                "scheme":      "?",
                "policy":      "?",
                "seed":        "?",
                "fault_model": "?",
                "due_action":  "?",
                "log":         os.path.join(run_dir, name),
                "exit":        0,
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


def wilson_ci(k: int, n: int, z: float = 1.959963984540054) -> Tuple[float, float, float]:
    """Wilson score CI for a binomial proportion at z (default 1.96, i.e. 95%).

    Returns (point_estimate, lo, hi). NaNs when n <= 0. The Wilson interval is
    preferred over the normal approximation because it stays well-behaved when
    k = 0, k = n, or n is small (which is exactly the regime where seed-mean
    +/- 1.96 sigma_p produces nonsense like negative lower bounds at low BER).
    """
    if n <= 0:
        return float("nan"), float("nan"), float("nan")
    k = max(0, min(k, n))
    p = k / n
    z2 = z * z
    denom = 1.0 + z2 / n
    center = (p + z2 / (2.0 * n)) / denom
    half   = (z / denom) * math.sqrt(p * (1.0 - p) / n + z2 / (4.0 * n * n))
    return p, max(0.0, center - half), min(1.0, center + half)


def _as_int(v) -> int:
    """Lenient int parse: empty/None/non-numeric -> 0. Used when pooling counts
    out of per_run_summary rows, some of which may have blank scorer columns."""
    if v in ("", None):
        return 0
    try:
        return int(float(v))
    except (TypeError, ValueError):
        return 0


def _as_float(v) -> float:
    if v in ("", None):
        return float("nan")
    try:
        return float(v)
    except (TypeError, ValueError):
        return float("nan")


def _policy_contrasts(pp_canonical: List[Dict],
                       per_run_rows: List[Dict],
                       canonical_fm: Optional[str],
                       canonical_due: Optional[str]) -> List[Dict]:
    """Emit per-(scheme, ber, policy) contrasts vs uniform on the canonical
    slice. The contrast is on pooled unsafe_action_rate so the CI uses the
    raw numerator/denominator counts, not the per-seed point estimates."""
    by_cell: Dict[Tuple[str, str], Dict[str, Tuple[int, int]]] = defaultdict(dict)
    for r in per_run_rows:
        if canonical_fm and r["fault_model"] != canonical_fm:
            continue
        if canonical_due and r["due_action"] != canonical_due:
            continue
        ftot = _as_int(r.get("frames_total", ""))
        funs = _as_int(r.get("frames_unsafe", ""))
        if ftot <= 0:
            continue
        cell = by_cell[(r["scheme"], r["ber"])]
        prev = cell.get(r["policy"], (0, 0))
        cell[r["policy"]] = (prev[0] + funs, prev[1] + ftot)

    out: List[Dict] = []
    for (scheme, ber), policy_map in sorted(by_cell.items()):
        if "uniform" not in policy_map:
            continue
        u_n, u_d = policy_map["uniform"]
        u_p, u_lo, u_hi = wilson_ci(u_n, u_d)
        for policy, (n, d) in policy_map.items():
            if policy == "uniform":
                continue
            p, lo, hi = wilson_ci(n, d)
            # Wald CI on the difference of two proportions; coarse but
            # sufficient for a "uniform - kernel_aware = +/- X" sentence.
            if u_d > 0 and d > 0:
                se = math.sqrt(p * (1.0 - p) / d + u_p * (1.0 - u_p) / u_d)
                diff = u_p - p
                z = 1.959963984540054
                diff_lo = diff - z * se
                diff_hi = diff + z * se
            else:
                diff = float("nan")
                diff_lo = float("nan")
                diff_hi = float("nan")
            out.append({
                "scheme":           scheme,
                "ber":              ber,
                "policy":           policy,
                "frames_total":     d,
                "frames_unsafe":    n,
                "unsafe_rate":      "" if math.isnan(p) else f"{p:.6e}",
                "uniform_unsafe":   "" if math.isnan(u_p) else f"{u_p:.6e}",
                "uniform_minus_policy":      "" if math.isnan(diff) else f"{diff:+.6e}",
                "uniform_minus_policy_lo":   "" if math.isnan(diff_lo) else f"{diff_lo:+.6e}",
                "uniform_minus_policy_hi":   "" if math.isnan(diff_hi) else f"{diff_hi:+.6e}",
            })
    return out


def _write_preflight_report(out_dir: str, *,
                             pp_canonical: List[Dict],
                             per_run_rows: List[Dict],
                             per_frame_rows: List[Dict],
                             rejections: List[Dict],
                             canonical_fm: Optional[str],
                             canonical_due: Optional[str],
                             unsafe_budget: float,
                             min_frames: int,
                             min_attributing_kernels: int) -> None:
    """Write analysis/preflight_report.txt. The report has a single
    'PREFLIGHT: PASS|FAIL' banner so a CI driver can grep for it; the body
    enumerates each gate the paper has to clear before any number is cited."""
    path = os.path.join(out_dir, "preflight_report.txt")

    # Gate 1: every cell on the canonical slice has frames_total >= min_frames.
    underpowered: List[Tuple[str, ...]] = []
    for r in pp_canonical:
        ftot = _as_int(r.get("frames_total", ""))
        if ftot < min_frames:
            underpowered.append((r["scheme"], r["policy"], r["ber"], str(ftot)))

    # Gate 2: at least one (scheme, policy) curve crosses the unsafe budget
    # on the upper Wilson bound. Required to lead with a "pressure point"
    # claim; otherwise the paper has to reframe around relative ordering.
    crossings: List[Tuple[str, str, str, str]] = []
    for r in pp_canonical:
        unsafe_hi = _as_float(r.get("unsafe_action_hi", ""))
        if not math.isnan(unsafe_hi) and unsafe_hi > unsafe_budget:
            crossings.append((r["scheme"], r["policy"], r["ber"],
                              f"{unsafe_hi:.3e}"))

    # Gate 3: rejected_runs.csv is empty (or has only acknowledged failures
    # like none+uniform segfaults at high BER, which a footnote in Table 1
    # is responsible for explaining).
    rejection_reasons: Dict[str, int] = defaultdict(int)
    for r in rejections:
        rejection_reasons[r["reason"]] += 1

    # Gate 4: among safety_violated==1 rows in per_frame_safety, count the
    # number of distinct attributing_kernel_name values. Tier B Fig. 3a
    # needs >= min_attributing_kernels for a useful bar chart. Falls back
    # to kernel_name when the simulator hasn't been rebuilt with the
    # attributing_kernel column yet (so old artifacts produce a 1-bar
    # report rather than a crash).
    attribution_col: Optional[str] = None
    for fr in per_frame_rows:
        if "attributing_kernel_name" in fr:
            attribution_col = "attributing_kernel_name"
            break
    if attribution_col is None and per_frame_rows:
        attribution_col = "kernel_name"
    distinct_attributing: set = set()
    if attribution_col:
        for fr in per_frame_rows:
            if canonical_fm and fr.get("fault_model") != canonical_fm:
                continue
            if canonical_due and fr.get("due_action") != canonical_due:
                continue
            if _as_int(fr.get("safety_violated", 0)) != 1:
                continue
            kn = fr.get(attribution_col, "")
            if kn:
                distinct_attributing.add(kn)

    # Gate 5: scorer-vs-VLA agreement on frames_dropped. After the
    # metric-integrity-gate fix, vla_frames_dropped_total should equal
    # scorer.frames_dropped for every per_run row (modulo type widening).
    # A divergence means an old binary or a stale analyzer is on the path.
    agreement_rows = 0
    disagreement_rows: List[Tuple[str, ...]] = []
    for r in per_run_rows:
        s_dropped = _as_int(r.get("frames_dropped", ""))
        v_dropped = _as_int(r.get("vla_frames_dropped_total", ""))
        if r.get("vla_frames_dropped_total", "") == "":
            continue  # no VLA block; nothing to check.
        agreement_rows += 1
        if s_dropped != v_dropped:
            disagreement_rows.append((r["scheme"], r["policy"], r["ber"],
                                      r["seed"], r["fault_model"],
                                      r["due_action"],
                                      str(s_dropped), str(v_dropped)))

    # Banner: the preflight passes when there are no underpowered cells, no
    # unexpected rejection reasons, and no scorer/VLA disagreement. The
    # crossing and attribution checks are reported but do not by themselves
    # fail the preflight (the paper can still be reframed around relative
    # ordering, and Tier B Fig. 3a is optional if attribution is gated to a
    # later artifact).
    expected_rejection_reasons = {"missing_log",
                                  "missing_scorer_summary"}
    unexpected_rejections = {k: v for k, v in rejection_reasons.items()
                             if not k.startswith("exit=")
                             and k not in expected_rejection_reasons
                             and not k.startswith("events_total<")
                             and not k.startswith("frames_total<")}
    passed = (not underpowered
              and not disagreement_rows
              and not unexpected_rejections)

    with open(path, "w") as f:
        f.write("=== ECC sweep preflight report ===\n")
        f.write(f"PREFLIGHT: {'PASS' if passed else 'FAIL'}\n\n")
        slice_str = (f"{canonical_fm}+{canonical_due}"
                     if canonical_fm else "(no slice; using full factorial)")
        f.write(f"canonical slice : {slice_str}\n")
        f.write(f"unsafe budget   : {unsafe_budget:.3e}\n")
        f.write(f"min frames/cell : {min_frames}\n")
        f.write(f"min attributing : {min_attributing_kernels}\n")
        f.write(f"cells (canonical): {len(pp_canonical)}\n\n")

        f.write("Gate 1: frames_total per cell >= min_frames\n")
        if not underpowered:
            f.write("  PASS: all canonical cells meet the floor.\n\n")
        else:
            f.write(f"  FAIL: {len(underpowered)} cell(s) underpowered:\n")
            for row in underpowered[:20]:
                f.write(f"    scheme={row[0]:<8} policy={row[1]:<13} "
                        f"ber={row[2]:<10} frames={row[3]}\n")
            if len(underpowered) > 20:
                f.write(f"    ... +{len(underpowered)-20} more\n")
            f.write("\n")

        f.write("Gate 2: at least one (scheme, policy) curve crosses unsafe_budget on its Wilson upper bound\n")
        if crossings:
            f.write(f"  PASS-LIKE: {len(crossings)} crossing(s):\n")
            for row in crossings[:10]:
                f.write(f"    scheme={row[0]:<8} policy={row[1]:<13} "
                        f"ber={row[2]:<10} unsafe_hi={row[3]}\n")
            if len(crossings) > 10:
                f.write(f"    ... +{len(crossings)-10} more\n")
            f.write("  -> paper may lead with a pressure-point claim.\n\n")
        else:
            f.write("  REFRAME: no crossing on the canonical slice.\n")
            f.write("  -> paper must lead with relative ordering at fixed stress BER\n")
            f.write("     (or extend BERS upward and re-run).\n\n")

        f.write("Gate 3: rejected_runs.csv has no unexpected reasons\n")
        if not rejection_reasons:
            f.write("  PASS: no rejected runs.\n\n")
        else:
            f.write("  Breakdown:\n")
            for reason, n in sorted(rejection_reasons.items(),
                                    key=lambda kv: -kv[1]):
                tag = "expected " if (reason in expected_rejection_reasons
                                       or reason.startswith("exit=")
                                       or reason.startswith("events_total<")
                                       or reason.startswith("frames_total<")) else "UNEXPECTED "
                f.write(f"    {tag}{reason:<28} : {n}\n")
            if unexpected_rejections:
                f.write("  FAIL: investigate the UNEXPECTED reasons above.\n\n")
            else:
                f.write("  PASS: only expected reasons (e.g., none+uniform "
                        "segfaults at high BER, exit codes from documented failures).\n\n")

        f.write("Gate 4: distinct attributing_kernel_name among safety_violated==1\n")
        if attribution_col == "attributing_kernel_name":
            f.write(f"  source: per_frame_safety.attributing_kernel_name\n")
        elif attribution_col == "kernel_name":
            f.write(f"  source: per_frame_safety.kernel_name (Tier B not yet built; expect 1 bar)\n")
        else:
            f.write(f"  source: per_frame_safety is empty.\n")
        f.write(f"  distinct kernels: {len(distinct_attributing)}\n")
        if distinct_attributing:
            f.write("  values: " + ", ".join(sorted(distinct_attributing)) + "\n")
        if len(distinct_attributing) >= min_attributing_kernels:
            f.write("  PASS-LIKE: meets the Tier B threshold.\n\n")
        else:
            f.write("  REFRAME: Fig. 3a will show fewer than the requested bars;\n")
            f.write("  treat Tier B as informational or re-run with the fix in place.\n\n")

        f.write("Gate 5: scorer.frames_dropped == VLA agent's frames_dropped_total\n")
        f.write(f"  rows audited: {agreement_rows}\n")
        if not disagreement_rows:
            f.write("  PASS: every audited row agreed.\n\n")
        else:
            f.write(f"  FAIL: {len(disagreement_rows)} row(s) disagreed:\n")
            for row in disagreement_rows[:10]:
                f.write("    scheme={} policy={} ber={} seed={} fm={} due={} scorer={} vla={}\n".format(*row))
            if len(disagreement_rows) > 10:
                f.write(f"    ... +{len(disagreement_rows)-10} more\n")
            f.write("\n")
    print(f"Wrote {path}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("run_dir", help="Directory containing per-run logs (and index.csv).")
    ap.add_argument("--out", default=None, help="Output directory (default: <run_dir>/analysis).")
    ap.add_argument("--deadline-ms", type=float, default=33.0,
                    help="Actuation deadline in ms; latencies above this count as violations.")
    ap.add_argument("--sdc-budget", type=float, default=1e-6,
                    help="Per-actuation SDC rate budget for the pressure-point line.")
    # Rejection thresholds: a run that never accumulated enough EccGuard events
    # (sim cap hit before traffic started, agent never published a kernel, etc.)
    # or never closed a frame (ACTUATE didn't fire) can't contribute a
    # meaningful row to per_run_summary / pressure_points. Dropping them keeps
    # the curves from being pulled toward "we ran a sim and saw nothing".
    ap.add_argument("--min-events", type=int, default=100,
                    help="Reject runs whose EccGuard events_total is below this. "
                         "Defaults to 100; raise for stricter filtering.")
    ap.add_argument("--min-frames", type=int, default=1,
                    help="Reject runs whose ActionScorer frames_total is below this. "
                         "Defaults to 1 (i.e., require at least one ACTUATE close).")
    # ISO 26262 / AEC-Q100 cross-reference: convert each cell's BER into the
    # equivalent FIT/Mbit/h figure using the same formula EccGuard derives in
    # reverse (see EccGuard.cc ~line 209). Default DRAM capacity and
    # ns-per-event match run_ecc_sweep.sh's defaults.
    ap.add_argument("--dram-capacity-mb", type=float, default=1024.0,
                    help="DRAM capacity in MiB used to compute fit_per_mbit_per_hour_equiv.")
    ap.add_argument("--sim-ns-per-event", type=float, default=100.0,
                    help="Wall-clock ns per simulated MemEvent for FIT inversion.")
    ap.add_argument("--line-bits", type=int, default=512,
                    help="Bits per cache line used to convert per-bit BER into a per-line "
                         "Poisson event rate (default 64B = 512 bits).")
    # Canonical-slice / preflight knobs (see plan section 7).
    ap.add_argument("--canonical-slice", default="",
                    help=("Restrict pressure_points.csv (and the preflight "
                          "report) to a single (fault_model, due_action) "
                          "tuple, e.g. 'jedec_mix+drop_frame'. The full "
                          "factorial is still emitted as "
                          "pressure_points_full.csv for the supplement."))
    ap.add_argument("--unsafe-budget", type=float, default=1e-6,
                    help=("Per-actuation safety budget on unsafe_action_rate "
                          "used by the preflight crossing check. Match the "
                          "value passed to make_figures.py."))
    ap.add_argument("--preflight-min-frames", type=int, default=24,
                    help=("Minimum frames_total per cell that the preflight "
                          "report flags as an underpowered Wilson CI. "
                          "Defaults to 24 (HEADLINE: 8 cycles * 3 seeds)."))
    ap.add_argument("--preflight-min-attributing-kernels", type=int, default=3,
                    help=("Minimum number of distinct attributing_kernel_name "
                          "values among safety_violated==1 frames required "
                          "for Tier B Fig. 3a. Reported in preflight."))
    ap.add_argument("--policy-contrasts", action="store_true",
                    help=("Also emit policy_contrasts.csv: per (scheme, ber) "
                          "the pooled unsafe_action_rate difference for each "
                          "non-uniform policy vs uniform, with a Wald 95% CI."))
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
    per_region_rows = []
    per_frame_rows  = []
    fault_mode_rows = []

    # The BER=0 row in run_ecc_sweep.sh is shared across all (fault_model,
    # due_action) cells for the same (scheme, policy, seed) - index.csv has
    # N entries pointing at the same log. Cache parses by absolute path so we
    # don't re-tokenize the same multi-thousand-line log N times.
    parse_cache: Dict[str, Dict] = {}
    rejections: List[Dict] = []

    for run in runs:
        if run["exit"] != 0 or not os.path.exists(run["log"]):
            rejections.append({**{k: run.get(k, "") for k in
                                  ("ber", "scheme", "policy", "seed",
                                   "fault_model", "due_action")},
                               "log": run["log"],
                               "events_total": "",
                               "frames_total": "",
                               "reason": ("missing_log" if not os.path.exists(run["log"])
                                          else f"exit={run['exit']}")})
            continue
        log_path = os.path.realpath(run["log"])
        parsed = parse_cache.get(log_path)
        if parsed is None:
            parsed = parse_run_log(log_path)
            parse_cache[log_path] = parsed
        e2e = end_to_end_ps(parsed["vla"])

        ecc_total_latency = sum(r["latency_ps"]   for r in parsed["ecc"])
        ecc_escape_total  = sum(r["escape"]       for r in parsed["ecc"])
        ecc_due_total     = sum(r["due"]          for r in parsed["ecc"])
        ecc_correct_total = sum(r["correctable"]  for r in parsed["ecc"])
        ecc_clean_total   = sum(r["clean"]        for r in parsed["ecc"])
        events_total      = (
            ecc_clean_total + ecc_correct_total + ecc_due_total + ecc_escape_total
        )

        scorer = parsed["scorer"] or {}
        es     = parsed["escape_summary"] or {}
        frames_total = scorer.get("frames_total", 0) or 0

        # Hard gate on scorer presence: previously we let runs through with no
        # ActionScorer summary block and silently fell back to the VLA agent's
        # cumulative frames_dropped_total, which mixed two different metric
        # definitions (per-frame edge-trigger vs. lifetime accumulator) and
        # produced pressure_points.csv rows that disagreed with
        # per_frame_safety.csv on the same cell. Now: missing scorer => reject.
        if parsed["scorer"] is None:
            rejections.append({
                "ber":          run["ber"],
                "scheme":       run["scheme"],
                "policy":       run["policy"],
                "seed":         run["seed"],
                "fault_model":  run["fault_model"],
                "due_action":   run["due_action"],
                "log":          run["log"],
                "events_total": events_total,
                "frames_total": frames_total,
                "reason":       "missing_scorer_summary",
            })
            continue

        # Threshold gate: a run with too few EccGuard events / ActionScorer
        # frames is statistically vacuous (the FSM didn't get going or the sim
        # cap fired before ACTUATE). Surface it in the rejection log and skip.
        if events_total < args.min_events or frames_total < args.min_frames:
            rejections.append({
                "ber":          run["ber"],
                "scheme":       run["scheme"],
                "policy":       run["policy"],
                "seed":         run["seed"],
                "fault_model":  run["fault_model"],
                "due_action":   run["due_action"],
                "log":          run["log"],
                "events_total": events_total,
                "frames_total": frames_total,
                "reason":       (f"events_total<{args.min_events}"
                                 if events_total < args.min_events
                                 else f"frames_total<{args.min_frames}"),
            })
            continue

        per_run_rows.append({
            "ber":                run["ber"],
            "scheme":             run["scheme"],
            "policy":             run["policy"],
            "seed":               run["seed"],
            "fault_model":        run["fault_model"],
            "due_action":         run["due_action"],
            "end_to_end_ps":      e2e if e2e is not None else "",
            "deadline_violated":  (1 if (e2e is not None and e2e > deadline_ps) else 0),
            "ecc_latency_ps":     ecc_total_latency,
            "events_total":       events_total,
            "events_clean":       ecc_clean_total,
            "events_correctable": ecc_correct_total,
            "events_due":         ecc_due_total,
            "events_escape":      ecc_escape_total,
            # Single source of truth for the per-frame metrics: the
            # ActionScorer summary block. We previously fell back to the VLA
            # agent's cumulative frames_dropped_total when the scorer was
            # absent or the field was missing, which produced pressure_points
            # rows showing nonzero drop_rate while per_frame_safety had
            # safety_violated == 0 for every frame. The scorer presence
            # check above guarantees this dict is populated.
            "frames_total":       scorer.get("frames_total", ""),
            "frames_dropped":     scorer.get("frames_dropped", ""),
            "frames_argmax_diff": scorer.get("frames_argmax_diff", ""),
            "frames_unsafe":      scorer.get("frames_unsafe", ""),
            "unsafe_action_rate": scorer.get("unsafe_action_rate", ""),
            "drop_rate":          scorer.get("drop_rate", ""),
            "argmax_change_rate": scorer.get("argmax_change_rate", ""),
            # Audit-only: cumulative drops as reported by the VLA agent.
            # Should match scorer.frames_dropped after the metric-integrity
            # gate; surfaced here so a divergence is visible in
            # per_run_summary.csv without having to re-parse the log.
            "vla_frames_dropped_total": parsed["frames_dropped_total"]
                                          if parsed["frames_dropped_total"] is not None else "",
            "escape_high_blast":  es.get("escape_high_blast", ""),
            "escape_low_blast":   es.get("escape_low_blast", ""),
            "frames_aborted":     es.get("frames_aborted", ""),
            "payload_dtype":      es.get("payload_dtype", ""),
            # Campaign-driver-only columns. Empty for plain sweep rows; for
            # campaign rows these carry the targeted-injection knobs so a
            # downstream consumer can pivot on (target_kernel,
            # campaign_mode) without re-parsing index.csv.
            "target_kernel":      run.get("target_kernel", ""),
            "campaign_mode":      run.get("campaign_mode", ""),
            "event_budget":       run.get("event_budget",  ""),
            "event_rate":         run.get("event_rate",    ""),
        })

        for r in parsed["ecc"]:
            per_kernel_rows.append({
                "ber":         run["ber"],
                "scheme":      run["scheme"],
                "policy":      run["policy"],
                "seed":        run["seed"],
                "fault_model": run["fault_model"],
                "due_action":  run["due_action"],
                "kernel_id":   r["kernel_id"],
                "kernel_name": r["kernel_name"],
                "clean":       r["clean"],
                "correctable": r["correctable"],
                "due":         r["due"],
                "escape":      r["escape"],
                "latency_ps":  r["latency_ps"],
            })

        for r in parsed["region"]:
            per_region_rows.append({
                "ber":         run["ber"],
                "scheme":      run["scheme"],
                "policy":      run["policy"],
                "seed":        run["seed"],
                "fault_model": run["fault_model"],
                "due_action":  run["due_action"],
                "kernel_id":   r["kernel_id"],
                "kernel_name": r["kernel_name"],
                "region":      r["region"],
                "clean":       r["clean"],
                "correctable": r["correctable"],
                "due":         r["due"],
                "escape":      r["escape"],
                "latency_ps":  r["latency_ps"],
            })

        for r in parsed["fault_modes"]:
            fault_mode_rows.append({
                "ber":         run["ber"],
                "scheme":      run["scheme"],
                "policy":      run["policy"],
                "seed":        run["seed"],
                "fault_model": run["fault_model"],
                "due_action":  run["due_action"],
                "mode":        r["mode"],
                "count":       r["count"],
            })

        for r in parsed["frames"]:
            per_frame_rows.append({
                "ber":                     run["ber"],
                "scheme":                  run["scheme"],
                "policy":                  run["policy"],
                "seed":                    run["seed"],
                "fault_model":             run["fault_model"],
                "due_action":              run["due_action"],
                "pipeline_cycle":          r["pipeline_cycle"],
                "kernel_at_close":         r["kernel_at_close"],
                "kernel_name":             r["kernel_name"],
                "attributing_kernel_id":   r.get("attributing_kernel_id",
                                                 r["kernel_at_close"]),
                "attributing_kernel_name": r.get("attributing_kernel_name",
                                                 r["kernel_name"]),
                "dropped":                 r["dropped"],
                "escapes_in_frame":        r["escapes_in_frame"],
                "flips_in_frame":          r["flips_in_frame"],
                "action_checksum":         r["action_checksum"],
                "golden_checksum":         r["golden_checksum"],
                "argmax_changed":          r["argmax_changed"],
                "safety_violated":         r["safety_violated"],
                "sim_time_ps":             r["sim_time_ps"],
            })

    def write(name, rows):
        if not rows:
            return
        path = os.path.join(out_dir, name)
        with open(path, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            writer.writeheader()
            writer.writerows(rows)

    write("per_run_summary.csv",     per_run_rows)
    write("per_kernel_overhead.csv", per_kernel_rows)
    write("per_region_overhead.csv", per_region_rows)
    write("per_frame_safety.csv",    per_frame_rows)
    write("fault_mode_mix.csv",      fault_mode_rows)
    write("rejected_runs.csv",       rejections)

    # Pressure-point curve: P50/P95, mean correction cost, deadline-violation
    # rate, SDC rate, drop_rate, argmax_change_rate, unsafe_action_rate.
    #
    # Two important correctness points:
    #   (1) Pool over raw events / frames before dividing. The previous code
    #       arithmetic-averaged per-seed unsafe_action_rate and drop_rate,
    #       which over-weights seeds that closed very few frames. We instead
    #       compute sum(numerator) / sum(denominator) across all seeds in the
    #       (scheme, policy, ber, fm, due) cell.
    #   (2) Attach a Wilson 95% CI to every binomial proportion so downstream
    #       plots can show error bars instead of bare means; with small N (e.g.
    #       5 seeds * 1 frame) the CIs are wide and that's the point.
    by_config: Dict[Tuple[str, str, str, str, str], List[Dict]] = defaultdict(list)
    for r in per_run_rows:
        if r["end_to_end_ps"] == "":
            continue
        by_config[(r["scheme"], r["policy"], r["ber"],
                   r["fault_model"], r["due_action"])].append(r)

    pp_rows = []
    for (scheme, policy, ber, fm, due), rows in sorted(by_config.items()):
        e2es      = [int(r["end_to_end_ps"]) for r in rows if r["end_to_end_ps"] != ""]
        latencies = [int(r["ecc_latency_ps"]) for r in rows]
        if not e2es:
            continue

        # Pool over events for SDC and frames for the scorer rates.
        events_total_sum   = sum(_as_int(r["events_total"])  for r in rows)
        events_escape_sum  = sum(_as_int(r["events_escape"]) for r in rows)
        frames_total_sum   = sum(_as_int(r["frames_total"])  for r in rows)
        frames_unsafe_sum  = sum(_as_int(r["frames_unsafe"]) for r in rows)
        frames_dropped_sum = sum(_as_int(r["frames_dropped"]) for r in rows)
        frames_argmax_sum  = sum(_as_int(r["frames_argmax_diff"]) for r in rows)
        viol_count         = sum(1 for v in e2es if v > deadline_ps)
        n_seeds            = len(e2es)

        viol_p,   viol_lo,   viol_hi   = wilson_ci(viol_count,         n_seeds)
        sdc_p,    sdc_lo,    sdc_hi    = wilson_ci(events_escape_sum,  events_total_sum)
        unsafe_p, unsafe_lo, unsafe_hi = wilson_ci(frames_unsafe_sum,  frames_total_sum)
        drop_p,   drop_lo,   drop_hi   = wilson_ci(frames_dropped_sum, frames_total_sum)
        argmax_p, argmax_lo, argmax_hi = wilson_ci(frames_argmax_sum,  frames_total_sum)

        # FIT/Mbit/h equivalent (for the iso-safety paragraph and Fig. 1's
        # secondary x-axis). The forward map in EccGuard.cc converts a FIT
        # rate into an event_rate via
        #     event_rate = (FIT * 1e-9 * dram_mb) / (3.6e12 / sim_ns_per_event).
        # We invert with the line-level event_rate ~= line_bits * BER (the
        # Poisson approximation; tight under EccScheme.h's BER bound). When
        # ber == 0 we report "" so the FIT axis on Fig. 1 doesn't try to take
        # log(0).
        try:
            ber_f = float(ber)
        except (TypeError, ValueError):
            ber_f = 0.0
        fit_equiv: str = ""
        if ber_f > 0.0 and args.dram_capacity_mb > 0:
            line_event_rate = args.line_bits * ber_f
            events_per_hour = 3.6e12 / args.sim_ns_per_event
            fit_equiv_val = (line_event_rate * events_per_hour) / (1e-9 * args.dram_capacity_mb)
            fit_equiv = "{:.3e}".format(fit_equiv_val)

        def _f(v: float, fmt: str = "{:.3e}") -> str:
            return "" if math.isnan(v) else fmt.format(v)

        pp_rows.append({
            "scheme":               scheme,
            "policy":               policy,
            "ber":                  ber,
            "fit_per_mbit_per_hour_equiv": fit_equiv,
            "fault_model":          fm,
            "due_action":           due,
            "n_seeds":              n_seeds,
            "events_total":         events_total_sum,
            "frames_total":         frames_total_sum,
            "p50_e2e_ps":           int(percentile(e2es, 50)),
            "p95_e2e_ps":           int(percentile(e2es, 95)),
            "mean_ecc_latency_ps":  int(sum(latencies) / max(len(latencies), 1)),
            "deadline_viol_rate":   _f(viol_p,   "{:.4f}"),
            "deadline_viol_lo":     _f(viol_lo,  "{:.4f}"),
            "deadline_viol_hi":     _f(viol_hi,  "{:.4f}"),
            "sdc_rate":             _f(sdc_p),
            "sdc_rate_lo":          _f(sdc_lo),
            "sdc_rate_hi":          _f(sdc_hi),
            "unsafe_action_rate":   _f(unsafe_p),
            "unsafe_action_lo":     _f(unsafe_lo),
            "unsafe_action_hi":     _f(unsafe_hi),
            "drop_rate":            _f(drop_p),
            "drop_rate_lo":         _f(drop_lo),
            "drop_rate_hi":         _f(drop_hi),
            "argmax_change_rate":   _f(argmax_p),
            "argmax_change_lo":     _f(argmax_lo),
            "argmax_change_hi":     _f(argmax_hi),
            "deadline_ms":          args.deadline_ms,
        })

    # Canonical-slice handling. The downstream paper figures only ever cite
    # one (fault_model, due_action) tuple to keep the curves un-overlapped;
    # everything else lives in the supplement. We persist the full factorial
    # as pressure_points_full.csv and write the slice (or the full set, when
    # no slice is requested) as pressure_points.csv -- the file all of
    # make_figures.py and table1_headline expect.
    canonical_fm: Optional[str] = None
    canonical_due: Optional[str] = None
    if args.canonical_slice:
        slice_str = args.canonical_slice.strip()
        if "+" not in slice_str:
            print(f"ERROR: --canonical-slice must be 'fault_model+due_action' "
                  f"(got '{slice_str}')", file=sys.stderr)
            return 2
        canonical_fm, canonical_due = slice_str.split("+", 1)

    pp_canonical: List[Dict] = pp_rows
    if canonical_fm and canonical_due:
        pp_canonical = [r for r in pp_rows
                        if r["fault_model"] == canonical_fm
                        and r["due_action"] == canonical_due]

    if pp_rows:
        # Always write the full factorial so the supplement / artifact has
        # everything; pressure_points.csv carries the slice the paper cites.
        full_path = os.path.join(
            out_dir,
            "pressure_points_full.csv" if canonical_fm else "pressure_points.csv")
        with open(full_path, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(pp_rows[0].keys()))
            writer.writeheader()
            writer.writerows(pp_rows)
    if canonical_fm and pp_canonical:
        with open(os.path.join(out_dir, "pressure_points.csv"), "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(pp_canonical[0].keys()))
            writer.writeheader()
            writer.writerows(pp_canonical)

    # Optional policy_contrasts.csv: pooled unsafe_action_rate difference
    # vs (uniform, same scheme) at each BER, with a Wald 95% CI on
    # the difference of proportions. Useful for the paper's "kernel_aware
    # buys you X +/- Y safety at BER=1e-5" sentence.
    if args.policy_contrasts and pp_canonical:
        contrast_rows = _policy_contrasts(pp_canonical, per_run_rows,
                                          canonical_fm, canonical_due)
        if contrast_rows:
            with open(os.path.join(out_dir, "policy_contrasts.csv"),
                      "w", newline="") as f:
                writer = csv.DictWriter(f, fieldnames=list(contrast_rows[0].keys()))
                writer.writeheader()
                writer.writerows(contrast_rows)

    # Preflight report: machine-readable yes/no on each gate the paper has
    # to clear before any number is cited. Emits both stdout-friendly text
    # and a one-line PASS/FAIL banner so a CI driver can grep for it.
    _write_preflight_report(
        out_dir,
        pp_canonical=pp_canonical,
        per_run_rows=per_run_rows,
        per_frame_rows=per_frame_rows,
        rejections=rejections,
        canonical_fm=canonical_fm,
        canonical_due=canonical_due,
        unsafe_budget=args.unsafe_budget,
        min_frames=args.preflight_min_frames,
        min_attributing_kernels=args.preflight_min_attributing_kernels,
    )

    summary_path = os.path.join(out_dir, "summary.txt")
    with open(summary_path, "w") as f:
        f.write("ECC pressure-point analysis\n")
        f.write("===========================\n")
        f.write(f"run_dir         : {os.path.abspath(args.run_dir)}\n")
        f.write(f"runs indexed    : {len(runs)}\n")
        f.write(f"runs ingested   : {len(per_run_rows)}\n")
        f.write(f"runs rejected   : {len(rejections)}"
                f" (min_events={args.min_events}, min_frames={args.min_frames})\n")
        f.write(f"deadline (ms)   : {args.deadline_ms}\n")
        f.write(f"sdc_budget      : {args.sdc_budget}\n\n")

        if rejections:
            # Top-line breakdown of rejection reasons; the full list lives in
            # rejected_runs.csv.
            by_reason: Dict[str, int] = defaultdict(int)
            for r in rejections:
                by_reason[r["reason"]] += 1
            f.write("Rejection breakdown:\n")
            for reason, n in sorted(by_reason.items(), key=lambda kv: -kv[1]):
                f.write(f"  {reason:<28} : {n}\n")
            f.write("(See rejected_runs.csv for full per-run reasons.)\n\n")

        f.write("Pressure-point table:\n")
        f.write("scheme  policy        ber          fm          due          P50ms   P95ms   ECCus   viol    sdc        unsafe\n")
        for r in pp_rows:
            unsafe = r["unsafe_action_rate"] or "-"
            f.write("{:<7} {:<13} {:<12} {:<11} {:<12} {:<7.3f} {:<7.3f} {:<7.3f} {:<7} {:<10} {}\n".format(
                r["scheme"], r["policy"], r["ber"], r["fault_model"], r["due_action"],
                r["p50_e2e_ps"] / 1e9, r["p95_e2e_ps"] / 1e9,
                r["mean_ecc_latency_ps"] / 1e6,
                r["deadline_viol_rate"], r["sdc_rate"], unsafe))
        f.write("\n")

        # Crossover scan: the first BER at which the lower bound of the
        # binomial CI clears the budget (instead of the point estimate) - that
        # way a wide CI at small N can't move the pressure point further out
        # than the data justifies.
        def _safe_float(s) -> float:
            try:
                return float(s)
            except (TypeError, ValueError):
                return 0.0

        f.write("Pressure points (first BER whose Wilson 95% lower bound clears budget):\n")
        groups: Dict[Tuple[str, str, str, str], List[Dict]] = defaultdict(list)
        for r in pp_rows:
            groups[(r["scheme"], r["policy"], r["fault_model"], r["due_action"])].append(r)
        for (scheme, policy, fm, due), rows in groups.items():
            try:
                rows_sorted = sorted(rows, key=lambda r: float(r["ber"]))
            except ValueError:
                rows_sorted = rows
            crossover = None
            for r in rows_sorted:
                viol_lo   = _safe_float(r.get("deadline_viol_lo", ""))
                sdc_lo    = _safe_float(r.get("sdc_rate_lo", ""))
                unsafe_lo = _safe_float(r.get("unsafe_action_lo", ""))
                if (viol_lo >= 0.05 or sdc_lo >= args.sdc_budget
                        or unsafe_lo >= args.sdc_budget):
                    crossover = r["ber"]
                    break
            f.write(f"  scheme={scheme:<8} policy={policy:<13} fm={fm:<10} due={due:<12} pressure_point_ber={crossover}\n")

    print(f"Wrote {summary_path}")
    if pp_rows:        print(f"Wrote {os.path.join(out_dir, 'pressure_points.csv')}")
    if per_run_rows:   print(f"Wrote {os.path.join(out_dir, 'per_run_summary.csv')}")
    if per_kernel_rows: print(f"Wrote {os.path.join(out_dir, 'per_kernel_overhead.csv')}")
    if per_region_rows: print(f"Wrote {os.path.join(out_dir, 'per_region_overhead.csv')}")
    if per_frame_rows:  print(f"Wrote {os.path.join(out_dir, 'per_frame_safety.csv')}")
    if fault_mode_rows: print(f"Wrote {os.path.join(out_dir, 'fault_mode_mix.csv')}")
    if rejections:
        print(f"Wrote {os.path.join(out_dir, 'rejected_runs.csv')} "
              f"({len(rejections)} run(s) below min_events={args.min_events}"
              f" or min_frames={args.min_frames})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
