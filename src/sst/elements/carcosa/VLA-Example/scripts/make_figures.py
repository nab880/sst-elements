#!/usr/bin/env python3
"""Render the cost--benefit study figures from the CSVs emitted by
analyze_ecc_results.py.

The figure layout matches the paper's Case Study A (sanity campaigns +
Pareto) and Case Study B (scheme/policy design-space study + sensitivity).

Inputs (in <analysis_dir>):
  - pressure_points.csv          (canonical slice if --canonical-slice was set)
  - pressure_points_full.csv     (full factorial; used for fig_due)
  - per_run_summary.csv
  - per_kernel_overhead.csv
  - per_region_overhead.csv
  - per_frame_safety.csv
  - fault_mode_mix.csv
  - case_study_table.csv         (only with --case-studies)

Outputs (under <analysis_dir>/figs/):
  fig_sanity.pdf            Case Study A: stacked outcome distribution (O1--O4)
                            per (scheme, campaign). Emitted by --case-studies.
  fig_pareto.pdf            Case Study A: cost--benefit Pareto frontier from
                            campaign C2 (mean ECC latency vs unsafe-action
                            rate). Emitted by --case-studies.
  fig_policy.pdf            Case Study B: policy granularity at fixed scheme.
                            Currently a blank placeholder; renderer pending.
  fig_attr.pdf              Case Study B: workload-visible escape attribution
                            by (kernel, region). Drawn from per_region_overhead
                            as the closest available proxy until a true
                            kernel x region heatmap renderer is wired up.
  fig_due.pdf               Case Study B: DUE-response trade-off, drop vs
                            deadline-miss across due_actions.
  fig_ber_sensitivity.pdf   Sensitivity: unsafe-action rate vs BER per
                            (scheme, policy). Demoted from headline; reported
                            for completeness, NOT as a deployment threshold.
  fig_fault_mode_mix.pdf    Supplementary: fraction of JEDEC fault modes vs BER.
  fig_attr_kernel.pdf       Supplementary: bar histogram of attributing_kernel
                            for safety-violated frames (legacy fig3a content).
  fig6_campaign_attribution.pdf
                            Supplementary: campaign-mode unsafe rate by target
                            kernel; only emitted when campaign rows exist.
  table1_headline.csv       One row per (scheme, policy) on the canonical
                            slice, with Wilson CIs.

Figures whose data is not yet wired up emit a blank placeholder PDF with a
title and a `placeholder` watermark so the LaTeX still compiles. The
placeholder helper is `_blank_placeholder()`.

Requires matplotlib + pandas. Both are intentionally light dependencies; the
script falls back to a clear error message if either is missing so that a
publication-time machine knows what to install.
"""

from __future__ import annotations

import argparse
import math
import os
import sys
from typing import Dict, List, Optional, Tuple

try:
    import pandas as pd
except ImportError:
    sys.stderr.write("make_figures.py: pandas required (pip install pandas)\n")
    sys.exit(2)

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    sys.stderr.write("make_figures.py: matplotlib required (pip install matplotlib)\n")
    sys.exit(2)


def _wilson_point(k: int, n: int, z: float = 1.959963984540054) -> float:
    """Wilson score point estimate for a binomial proportion."""
    if n <= 0:
        return float("nan")
    p_hat = k / n
    denom = 1.0 + z * z / n
    return (p_hat + z * z / (2.0 * n)) / denom


def _read_csv(path: str) -> pd.DataFrame:
    if not os.path.exists(path):
        return pd.DataFrame()
    return pd.read_csv(path)


def _ensure_dir(p: str) -> str:
    os.makedirs(p, exist_ok=True)
    return p


def _blank_placeholder(out_dir: str, filename: str, title: str,
                        note: str = "") -> None:
    """Emit a blank placeholder PDF so the LaTeX paper compiles before the
    real figure-rendering logic for `filename` is wired up.

    The placeholder carries a bold title, an optional note, and a
    `placeholder` watermark so a reader (or a CI artifact diff) cannot
    mistake it for a finished figure.
    """
    fig, ax = plt.subplots(figsize=(6.0, 4.0))
    ax.text(0.5, 0.62, title, ha="center", va="center",
            fontsize=12, fontweight="bold", transform=ax.transAxes)
    if note:
        ax.text(0.5, 0.46, note, ha="center", va="center",
                fontsize=9, wrap=True, transform=ax.transAxes)
    ax.text(0.5, 0.18, "(placeholder; renderer pending)",
            ha="center", va="center", fontsize=8, style="italic",
            color="#888888", transform=ax.transAxes)
    ax.set_axis_off()
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, filename))
    plt.close(fig)


def _ci_yerr(df: pd.DataFrame, point: str, lo: str, hi: str):
    """Convert (point, lo, hi) columns into a (2, N) ndarray suitable for
    matplotlib's `yerr=` argument: row 0 = below-bar deltas, row 1 = above.
    Missing CIs collapse to zero error bars so the point still plots."""
    p  = pd.to_numeric(df[point], errors="coerce")
    lo = pd.to_numeric(df.get(lo, p), errors="coerce")
    hi = pd.to_numeric(df.get(hi, p), errors="coerce")
    below = (p - lo).clip(lower=0).fillna(0.0).to_numpy()
    above = (hi - p).clip(lower=0).fillna(0.0).to_numpy()
    return [below, above]


def select_iso_ber(df: pd.DataFrame, scheme: str, policy: str,
                    budget: float) -> "Optional[float]":
    """Return the iso-safety BER for one (scheme, policy) curve.

    Definition (cited verbatim in the paper appendix and in --help):
        iso_ber(scheme, policy, budget) = max { ber : unsafe_action_rate(ber) <= budget }
    over the rows in `df` that match `scheme` and `policy`. Returns None
    when no row meets the budget (i.e., the curve never crossed below the
    budget on the BER grid sampled). Used by Fig. 1 annotations and by
    the Fig. 2 bars so both figures cite the same row of pressure_points.csv.
    """
    if df.empty:
        return None
    cell = df[(df["scheme"] == scheme) & (df["policy"] == policy)].copy()
    if cell.empty:
        return None
    cell["ber_f"]   = pd.to_numeric(cell["ber"], errors="coerce")
    cell["unsafe"]  = pd.to_numeric(cell["unsafe_action_rate"], errors="coerce")
    safe = cell[(cell["unsafe"].notna()) & (cell["unsafe"] <= budget)]
    if safe.empty:
        return None
    return float(safe["ber_f"].max())


def select_iso_ber_table(df: pd.DataFrame, budget: float) -> pd.DataFrame:
    """Compute the iso-safety BER for every (scheme, policy) pair present
    in `df`. Returns a DataFrame with columns scheme, policy, iso_ber
    (NaN when no crossing); used by Fig. 2 to look up the latency row.
    """
    if df.empty:
        return pd.DataFrame(columns=["scheme", "policy", "iso_ber"])
    rows = []
    for (scheme, policy), _ in df.groupby(["scheme", "policy"]):
        rows.append({
            "scheme":  scheme,
            "policy":  policy,
            "iso_ber": select_iso_ber(df, scheme, policy, budget),
        })
    return pd.DataFrame(rows)


def fig_ber_sensitivity(pp: pd.DataFrame, out_dir: str,
                         unsafe_budget: float) -> None:
    """Sensitivity: unsafe-action rate vs BER per (scheme, policy), faceted
    by scheme. BER is treated as a controlled stress axis under which the
    cross-design-point ordering is evaluated; the absolute BER values are
    NOT interpreted as a deployment threshold (see paper Sec. Case Study B).
    The chosen unsafe-action budget is drawn as a horizontal reference line
    and the iso-budget BER per curve is highlighted for traceability into
    Fig. fig_pareto.

    Note: pp is expected to be the canonical slice (one fault_model x
    one due_action). The analyzer's --canonical-slice flag enforces this;
    if the caller passed the full factorial we still plot, but curves
    will overlap and the reader cannot tell which fault_model produced
    which point.
    """
    if pp.empty:
        _blank_placeholder(out_dir, "fig_ber_sensitivity.pdf",
                           "Sensitivity: unsafe-action rate vs BER",
                           "no pressure_points.csv rows on the canonical slice")
        return
    df = pp.copy()
    df["ber_f"] = pd.to_numeric(df["ber"], errors="coerce")
    df_log = df.dropna(subset=["ber_f"]).copy()
    df_log = df_log[df_log["ber_f"] > 0]  # log-x can't render BER=0
    df_log["unsafe_action_rate"] = pd.to_numeric(
        df_log["unsafe_action_rate"], errors="coerce").fillna(0.0)
    if df_log.empty:
        return

    schemes = sorted(df_log["scheme"].unique().tolist())
    n = max(1, len(schemes))
    fig, axes = plt.subplots(1, n, figsize=(3.6 * n, 3.6),
                              sharey=True, squeeze=False)
    iso_table = select_iso_ber_table(df, unsafe_budget)

    for ax, scheme in zip(axes[0], schemes):
        sub = df_log[df_log["scheme"] == scheme]
        for policy, grp in sub.groupby("policy"):
            grp = grp.sort_values("ber_f")
            yerr = _ci_yerr(grp, "unsafe_action_rate",
                            "unsafe_action_lo", "unsafe_action_hi")
            ax.errorbar(grp["ber_f"], grp["unsafe_action_rate"],
                        yerr=yerr, marker="o", capsize=3, linewidth=1.2,
                        label=policy)
            # Highlight the iso-safety row when one exists.
            iso_row = iso_table[(iso_table["scheme"] == scheme)
                                & (iso_table["policy"] == policy)]
            if not iso_row.empty:
                iso_ber = iso_row.iloc[0]["iso_ber"]
                if iso_ber is not None and not pd.isna(iso_ber) and iso_ber > 0:
                    pt = grp[grp["ber_f"] == iso_ber]
                    if not pt.empty:
                        ax.scatter(pt["ber_f"], pt["unsafe_action_rate"],
                                   marker="*", s=80, zorder=4, edgecolor="k")

        # The caption already promises a budget line; draw it.
        ax.axhline(unsafe_budget, linestyle="--", linewidth=1.0,
                   alpha=0.7, label=f"unsafe budget = {unsafe_budget:g}")
        ax.set_xscale("log")
        ax.set_yscale("symlog", linthresh=1e-9)
        ax.set_xlabel("BER (per-bit per access)")
        ax.set_title(f"scheme = {scheme}")
        ax.grid(True, which="both", linestyle=":", alpha=0.5)
        ax.legend(fontsize=7, loc="best")
    axes[0][0].set_ylabel("Unsafe-action rate (per actuation, Wilson 95% CI)")

    # Top axis on the rightmost facet: FIT/Mbit/h cross-reference, mirrored
    # off the analyzer-emitted fit_per_mbit_per_hour_equiv column so the
    # conversion stays consistent with EccGuard.cc's forward formula.
    if "fit_per_mbit_per_hour_equiv" in df_log.columns:
        fit_series = pd.to_numeric(df_log["fit_per_mbit_per_hour_equiv"],
                                    errors="coerce")
        ber_series = df_log["ber_f"]
        valid = fit_series.notna() & (fit_series > 0) & (ber_series > 0)
        if valid.any():
            scale = (fit_series[valid] / ber_series[valid]).median()

            def ber_to_fit(b):
                return b * scale

            def fit_to_ber(f):
                return f / scale if scale > 0 else f

            ax2 = axes[0][-1].secondary_xaxis(
                "top", functions=(ber_to_fit, fit_to_ber))
            ax2.set_xlabel("FIT / Mbit / hour")
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "fig_ber_sensitivity.pdf"))
    plt.close(fig)


def fig_attr_kernel(per_frame: pd.DataFrame,
                     out_dir: str,
                     canonical_fm: str = "jedec_mix",
                     canonical_due: str = "drop_frame") -> None:
    """Supplementary kernel-only attribution: among scorer-flagged
    safety_violated == 1 frames, group by attributing_kernel_name (the
    kernel that produced the most escapes in that frame; populated by
    the simulator). Falls back to kernel_name (== ACTUATE for every
    frame on the legacy build) so old artifacts still render -- but
    the caption needs to call out the fallback when only one bar is
    visible.

    The headline (kernel x region) attribution figure is fig_attr.pdf
    (rendered from per_region_overhead via fig_attr); this function
    is the supplementary kernel-only bar plot, written to
    fig_attr_kernel.pdf.
    """
    if per_frame.empty:
        _blank_placeholder(out_dir, "fig_attr_kernel.pdf",
                           "Supplementary: kernel-only escape attribution",
                           "no per_frame_safety.csv rows")
        return
    df = per_frame.copy()
    if "fault_model" in df.columns and "due_action" in df.columns:
        df = df[(df["fault_model"] == canonical_fm) & (df["due_action"] == canonical_due)]
    df = df[pd.to_numeric(df["safety_violated"], errors="coerce").fillna(0) == 1]
    if df.empty:
        fig, ax = plt.subplots(figsize=(7.0, 4.0))
        ax.text(
            0.5, 0.5,
            "No per-frame safety_violated==1 rows\n"
            "on the canonical slice.\n"
            "(Run-level deadline_viol_rate may still be 1.0.)",
            ha="center", va="center", fontsize=10, wrap=True,
        )
        ax.set_axis_off()
        ax.set_title("Supplementary: kernel-only attribution (no rows)")
        fig.tight_layout()
        fig.savefig(os.path.join(out_dir, "fig_attr_kernel.pdf"))
        plt.close(fig)
        return

    # Prefer the simulator-emitted attribution column; fall back to
    # kernel_name (always ACTUATE on legacy artifacts) so this still
    # produces a figure rather than crashing.
    if "attributing_kernel_name" in df.columns:
        col = "attributing_kernel_name"
        title_suffix = "(attributing_kernel = max escapes_in_frame on this cycle)"
    else:
        col = "kernel_name"
        title_suffix = "(legacy artifact: kernel_at_close, ACTUATE-only)"

    counts = df.groupby(col).size().sort_values(ascending=False).head(10)
    fig, ax = plt.subplots(figsize=(7.0, 4.0))
    ax.bar(counts.index.astype(str), counts.values)
    ax.set_ylabel("Safety-violated frames (count)")
    ax.set_title("Supplementary: kernel-only escape attribution\n" + title_suffix)
    plt.setp(ax.get_xticklabels(), rotation=35, ha="right")
    ax.grid(True, axis="y", linestyle=":", alpha=0.5)
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "fig_attr_kernel.pdf"))
    plt.close(fig)


def per_region_for_canonical(per_region: pd.DataFrame,
                              per_frame: pd.DataFrame) -> pd.DataFrame:
    """Filter per_region_overhead.csv to the canonical (fault_model,
    due_action) implied by per_frame_safety.csv. We use per_frame as the
    arbiter because pressure_points.csv has already been sliced upstream
    and the per-frame file inherits that slice via the analyzer.
    Returns the original frame unchanged when no slice can be inferred."""
    if per_region.empty:
        return per_region
    if per_frame.empty or "fault_model" not in per_frame.columns:
        return per_region
    fms = per_frame["fault_model"].dropna().unique().tolist()
    dus = per_frame["due_action"].dropna().unique().tolist()
    if len(fms) == 1 and len(dus) == 1 and "fault_model" in per_region.columns:
        return per_region[(per_region["fault_model"] == fms[0])
                          & (per_region["due_action"] == dus[0])].copy()
    return per_region


def fig_attr(per_region: pd.DataFrame, out_dir: str) -> None:
    """Headline (kernel x region) escape attribution. Renders a heatmap
    of EccGuard escape counts indexed by (kernel_name, region) on the
    canonical slice. Until a true 2-D heatmap renderer is wired up,
    this falls back to a stacked-bar approximation so the figure still
    carries the (kernel, region) signal.

    Caveat: an escape at the EccGuard layer and a frame flagged
    safety_violated by the scorer are two different layers
    (DRAM/ECC layer vs behavioral layer). The caption notes this; see
    paper appendix glossary.
    """
    if per_region.empty:
        _blank_placeholder(out_dir, "fig_attr.pdf",
                           "Workload-visible escape attribution",
                           "no per_region_overhead.csv rows; "
                           "rerun with VLA_REGIONS configured")
        return
    df = per_region.copy()
    if "escape" not in df.columns:
        _blank_placeholder(out_dir, "fig_attr.pdf",
                           "Workload-visible escape attribution",
                           "per_region_overhead.csv missing 'escape' column")
        return
    df["escape"] = pd.to_numeric(df["escape"], errors="coerce").fillna(0)
    df = df[df["escape"] > 0]
    if df.empty:
        _blank_placeholder(out_dir, "fig_attr.pdf",
                           "Workload-visible escape attribution",
                           "no escape rows on the canonical slice "
                           "(framework correctly suppressed all events)")
        return

    bucket = df.groupby(["kernel_name", "region"])["escape"].sum().reset_index()
    bucket = bucket.sort_values("escape", ascending=False)
    # Cap to top-N (kernel, region) buckets to keep the bar readable;
    # roll the rest into "(other)". Separate UNKNOWN/unlabeled because it
    # tags writebacks without a vAddr (footnote in caption).
    TOP_N = 12
    head = bucket.head(TOP_N).copy()
    tail_total = bucket.iloc[TOP_N:]["escape"].sum() if len(bucket) > TOP_N else 0
    if tail_total > 0:
        head = pd.concat([head, pd.DataFrame([{
            "kernel_name": "(other)", "region": "", "escape": tail_total,
        }])], ignore_index=True)
    head["label"] = head.apply(
        lambda r: f"{r['kernel_name']}@{r['region']}" if r["region"]
                   else f"{r['kernel_name']}", axis=1)

    fig, ax = plt.subplots(figsize=(7.5, 4.0))
    ax.bar(head["label"], head["escape"])
    ax.set_ylabel("EccGuard escape classifications (count)")
    ax.set_title("Workload-visible escape attribution by (kernel, region)\n"
                 "(EccGuard-layer escapes; see paper appendix glossary)")
    plt.setp(ax.get_xticklabels(), rotation=40, ha="right", fontsize=8)
    ax.grid(True, axis="y", linestyle=":", alpha=0.5)
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "fig_attr.pdf"))
    plt.close(fig)


def fig_fault_mode_mix(fm: pd.DataFrame, out_dir: str) -> None:
    """Supplementary: fraction of JEDEC fault modes vs BER. Reported for
    transparency on what the JEDEC sampler is producing at each stress
    level."""
    if fm.empty:
        _blank_placeholder(out_dir, "fig_fault_mode_mix.pdf",
                           "JEDEC fault-mode mix vs BER",
                           "no fault_mode_mix.csv rows")
        return
    df = fm[fm["fault_model"] == "jedec_mix"].copy()
    if df.empty:
        _blank_placeholder(out_dir, "fig_fault_mode_mix.pdf",
                           "JEDEC fault-mode mix vs BER",
                           "no jedec_mix rows in fault_mode_mix.csv")
        return
    df["ber_f"] = pd.to_numeric(df["ber"], errors="coerce")
    df = df.dropna(subset=["ber_f"])
    if df.empty:
        return
    pivot = df.pivot_table(index="ber_f", columns="mode", values="count",
                           aggfunc="sum", fill_value=0)
    norm = pivot.div(pivot.sum(axis=1), axis=0)
    norm = norm.sort_index()

    fig, ax = plt.subplots(figsize=(6.5, 4.0))
    ax.stackplot(norm.index, norm.T.values, labels=norm.columns)
    ax.set_xscale("log")
    ax.set_xlabel("BER (per-bit per access)")
    ax.set_ylabel("Fraction of fault events")
    ax.set_title("JEDEC fault-mode mix vs BER")
    ax.legend(fontsize=7, loc="upper left", bbox_to_anchor=(1.02, 1.0))
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "fig_fault_mode_mix.pdf"))
    plt.close(fig)


def fig_due(pp_full: pd.DataFrame, out_dir: str) -> None:
    """DUE-response trade-off: frame-drop vs deadline-miss split by
    due_action. The headline canonical slice locks the main-text axes
    to drop_frame, so this figure only carries content when the full
    factorial (FULL_CUBE artifact, or a custom run) includes both
    `latency_only` and `drop_frame`."""
    if pp_full.empty:
        _blank_placeholder(out_dir, "fig_due.pdf",
                           "DUE-response trade-off (drop vs deadline)",
                           "no pressure_points_full.csv rows")
        return
    df = pp_full.copy()
    df["ber_f"] = pd.to_numeric(df["ber"], errors="coerce")
    df["drop_rate"] = pd.to_numeric(df["drop_rate"], errors="coerce").fillna(0.0)
    df["deadline_viol_rate"] = pd.to_numeric(df["deadline_viol_rate"], errors="coerce")
    df = df.dropna(subset=["ber_f"])
    if df.empty:
        _blank_placeholder(out_dir, "fig_due.pdf",
                           "DUE-response trade-off (drop vs deadline)",
                           "no usable rows after BER coercion")
        return

    fig, ax = plt.subplots(figsize=(6.5, 4.0))
    for (due,), grp in df.groupby(["due_action"]):
        grp = grp.sort_values("ber_f")
        ax.errorbar(grp["ber_f"], grp["drop_rate"],
                    yerr=_ci_yerr(grp, "drop_rate", "drop_rate_lo", "drop_rate_hi"),
                    marker="o", linestyle="-", capsize=3,
                    label=f"drop_rate ({due})")
        ax.errorbar(grp["ber_f"], grp["deadline_viol_rate"],
                    yerr=_ci_yerr(grp, "deadline_viol_rate",
                                  "deadline_viol_lo", "deadline_viol_hi"),
                    marker="x", linestyle="--", capsize=3,
                    label=f"deadline_viol ({due})")
    ax.set_xscale("log")
    ax.set_xlabel("BER (per-bit per access)")
    ax.set_ylabel("Rate (Wilson 95% CI)")
    ax.set_title("DUE-response trade-off: drop vs deadline-miss by due_action")
    ax.grid(True, which="both", linestyle=":", alpha=0.5)
    ax.legend(fontsize=7, loc="best")
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "fig_due.pdf"))
    plt.close(fig)


def fig_policy(pp: pd.DataFrame, out_dir: str) -> None:
    """Policy granularity at fixed scheme: bars of workload-level
    benefit and cost across {uniform, kernel_aware, region_aware, full}
    for each scheme. Currently a blank placeholder; the data lives in
    pressure_points.csv but the comparison renderer is pending --
    leaving blank per the framework's stub policy."""
    _blank_placeholder(out_dir, "fig_policy.pdf",
                       "Policy granularity at fixed scheme",
                       ("uniform / kernel_aware / region_aware / full "
                        "comparison;\nrenderer pending."))


def table1_headline(pp: pd.DataFrame, out_dir: str) -> None:
    if pp.empty:
        return
    df = pp.copy()
    df["mean_ecc_latency_us"] = pd.to_numeric(df["mean_ecc_latency_ps"], errors="coerce") / 1e6
    df = df.sort_values(["scheme", "policy", "ber"])
    # Carry through the Wilson CI bounds so headline numbers in the paper
    # always travel with their uncertainty - no more bare point estimates.
    cols = ["scheme", "policy", "ber", "fit_per_mbit_per_hour_equiv",
            "fault_model", "due_action",
            "n_seeds", "events_total", "frames_total",
            "mean_ecc_latency_us",
            "deadline_viol_rate", "deadline_viol_lo", "deadline_viol_hi",
            "sdc_rate", "sdc_rate_lo", "sdc_rate_hi",
            "drop_rate", "drop_rate_lo", "drop_rate_hi",
            "argmax_change_rate", "argmax_change_lo", "argmax_change_hi",
            "unsafe_action_rate", "unsafe_action_lo", "unsafe_action_hi"]
    # Tolerate older pressure_points.csv files (missing CI columns) by only
    # writing the columns that actually exist.
    cols = [c for c in cols if c in df.columns]
    df[cols].to_csv(os.path.join(out_dir, "table1_headline.csv"), index=False)


def fig_sanity(table: pd.DataFrame, out_dir: str) -> None:
    """Case Study A sanity campaigns: stacked outcome fractions (O1--O4)
    by campaign, faceted by scheme. The figure is the framework's
    behavioural sanity check (paper Sec. Case Study A)."""
    import matplotlib.pyplot as plt

    if table.empty or "case_id" not in table.columns:
        _blank_placeholder(out_dir, "fig_sanity.pdf",
                           "Case Study A: sanity campaigns C0--C4",
                           "no case_study_table.csv rows; "
                           "rerun with run_case_studies.sh")
        return
    df = table[table["latency_profile"].fillna("default") == "default"].copy()
    if df.empty:
        df = table.copy()
    schemes = [s for s in ["none", "secded", "chipkill"] if s in df["scheme"].values]
    cases = sorted(df["case_id"].unique())
    fig, axes = plt.subplots(1, len(schemes), figsize=(4 * len(schemes), 4), sharey=True)
    if len(schemes) == 1:
        axes = [axes]
    colors = {"O1": "#2ca02c", "O2": "#ff7f0e", "O3": "#d62728", "O4": "#9467bd"}
    stacks = [("O1", "fraction_O1"), ("O2", "fraction_O2"),
              ("O3", "fraction_O3"), ("O4", "fraction_O4")]
    for ax, sch in zip(axes, schemes):
        sub = df[df["scheme"] == sch]
        for i, c in enumerate(cases):
            m = sub[sub["case_id"] == c]
            bottom = 0.0
            for oc, col in stacks:
                v = float(m[col].mean()) if not m.empty and col in m.columns else 0.0
                ax.bar(i, v, bottom=bottom, color=colors[oc], width=0.7,
                       label=oc if (i == 0 and sch == schemes[0]) else "")
                bottom += v
        ax.set_xticks(range(len(cases)))
        ax.set_xticklabels(cases, rotation=25, ha="right")
        ax.set_title(sch)
        ax.set_ylim(0, 1.05)
    axes[0].set_ylabel("fraction of frames")
    handles, labels = axes[0].get_legend_handles_labels()
    if handles:
        fig.legend(handles, labels, loc="upper right", fontsize=7)
    fig.suptitle("Sanity campaigns C0--C4: outcome mix (default latency)")
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "fig_sanity.pdf"))
    plt.close(fig)


def fig_pareto(table: pd.DataFrame, out_dir: str) -> None:
    """Cost--benefit Pareto frontier from campaign C2 (the most
    informative single campaign because it separates SECDED and
    ChipKill). x-axis: mean ECC latency (cost). y-axis:
    unsafe_action_rate (benefit; lower is better). One point per
    (scheme, latency_profile)."""
    import matplotlib.pyplot as plt

    df = table[table["case_id"] == "C2"].copy()
    if df.empty:
        _blank_placeholder(out_dir, "fig_pareto.pdf",
                           "Cost--benefit Pareto frontier (C2)",
                           "no C2 rows in case_study_table.csv; "
                           "rerun run_case_studies.sh")
        return
    fig, ax = plt.subplots(figsize=(5, 4))
    for _, r in df.iterrows():
        label = f"{r['scheme']}@{r.get('latency_profile', 'default')}"
        ax.scatter(float(r["ecc_latency_ps"]) / 1e6,
                   float(r["unsafe_action_rate"]),
                   label=label, s=60)
    ax.set_xlabel("mean ECC latency (ms)")
    ax.set_ylabel("unsafe_action_rate (lower is better)")
    ax.set_title("Cost--benefit Pareto frontier (campaign C2)")
    ax.legend(fontsize=7)
    ax.grid(True, linestyle=":", alpha=0.5)
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "fig_pareto.pdf"))
    plt.close(fig)


def table1_case_study(table: pd.DataFrame, analysis_dir: str) -> None:
    path = os.path.join(analysis_dir, "table1_case_study.csv")
    cols = ["case_id", "scheme", "policy", "latency_profile", "unsafe_action_rate",
            "unsafe_lo", "unsafe_hi", "fraction_O1", "fraction_O2", "fraction_O3",
            "fraction_O4", "ecc_latency_ps"]
    cols = [c for c in cols if c in table.columns]
    table[cols].to_csv(path, index=False)
    print(f"Wrote {path}")


def fig6_campaign_attribution(per_run: pd.DataFrame, out_dir: str) -> None:
    """Campaign-mode mechanistic figure. Filters per_run_summary.csv to
    rows with fault_model == 'campaign' (rows added by run_ecc_campaign.sh)
    and renders unsafe-action rate by (target_kernel, scheme, policy)
    grouped bars. The point of the figure is to read off how much of
    the safety budget a chosen kernel actually consumes when an
    *explicit* fault budget is aimed at it -- complementary to the
    statistical pressure-point figures.

    Skips silently when no campaign rows are present (most artifact
    directories will only carry sweep rows).
    """
    if per_run.empty:
        return
    if "fault_model" not in per_run.columns:
        return
    df = per_run[per_run["fault_model"] == "campaign"].copy()
    if df.empty or "target_kernel" not in df.columns:
        return

    # Aggregate seeds via Wilson-pooled frames_unsafe / frames_total,
    # consistent with how pressure_points are built. Falls back to
    # arithmetic mean if frame counts are missing.
    for col in ("frames_unsafe", "frames_total"):
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce")
    df["unsafe_action_rate"] = pd.to_numeric(df["unsafe_action_rate"],
                                              errors="coerce")
    df = df.dropna(subset=["unsafe_action_rate"])
    if df.empty:
        return

    if "frames_unsafe" in df.columns and "frames_total" in df.columns:
        agg = (df.groupby(["target_kernel", "scheme", "policy"])
                 .agg(frames_unsafe=("frames_unsafe", "sum"),
                      frames_total=("frames_total", "sum"))
                 .reset_index())
        agg["unsafe_action_rate"] = agg.apply(
            lambda r: _wilson_point(int(r["frames_unsafe"]),
                                    int(r["frames_total"])), axis=1)
        grouped = agg
    else:
        grouped = (df.groupby(["target_kernel", "scheme", "policy"])
                     ["unsafe_action_rate"].mean()
                     .reset_index())

    # One subplot per (scheme, policy), x = target_kernel. Keep the
    # figure small; campaigns typically have ~5 targets and 2-3 schemes.
    targets = list(grouped["target_kernel"].unique())
    pairs   = list(grouped[["scheme", "policy"]].drop_duplicates().itertuples(index=False, name=None))
    if not targets or not pairs:
        return

    fig, ax = plt.subplots(figsize=(7.0, 4.0))
    width = 0.8 / max(len(pairs), 1)
    xs = list(range(len(targets)))
    for i, (sch, pol) in enumerate(pairs):
        ys = []
        for t in targets:
            m = grouped[(grouped["target_kernel"] == t)
                        & (grouped["scheme"] == sch)
                        & (grouped["policy"] == pol)]
            ys.append(float(m["unsafe_action_rate"].iloc[0]) if not m.empty else 0.0)
        offsets = [x - 0.4 + i * width + width / 2 for x in xs]
        ax.bar(offsets, ys, width=width, label=f"{sch}/{pol}")
    ax.set_xticks(xs)
    ax.set_xticklabels(targets, rotation=30, ha="right")
    ax.set_ylabel("unsafe_action_rate")
    ax.set_title("Fig. 6 Campaign-mode unsafe-action rate by target kernel")
    ax.legend(fontsize=7, loc="upper left", bbox_to_anchor=(1.02, 1.0))
    ax.grid(True, axis="y", linestyle=":", alpha=0.5)
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "fig6_campaign_attribution.pdf"))
    plt.close(fig)


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("analysis_dir", help="Output dir from analyze_ecc_results.py.")
    ap.add_argument("--case-studies", action="store_true",
                    help="Render fig_case_study_grid + fig_pareto_case_C2 from case_study_table.csv.")
    ap.add_argument("--unsafe-budget", type=float, default=1e-6,
                    help=("Unsafe-action rate budget for the iso-safety figure "
                          "and the budget reference line on Fig. 1. Same value "
                          "should be passed to analyze_ecc_results.py "
                          "--unsafe-budget so the preflight crossing check uses "
                          "the same threshold."))
    args = ap.parse_args()

    if not os.path.isdir(args.analysis_dir):
        sys.stderr.write(f"ERROR: {args.analysis_dir} is not a directory.\n")
        return 2

    if args.case_studies:
        table_path = os.path.join(args.analysis_dir, "case_study_table.csv")
        if not os.path.isfile(table_path):
            sys.stderr.write(f"ERROR: missing {table_path}\n")
            return 2
        table = _read_csv(table_path)
        figs_dir = _ensure_dir(os.path.join(args.analysis_dir, "figs"))
        fig_sanity(table, figs_dir)
        fig_pareto(table, figs_dir)
        table1_case_study(table, args.analysis_dir)
        print(f"Case Study A figures (fig_sanity, fig_pareto) "
              f"written under {figs_dir}")
        return 0

    # Canonical-slice file layout: when the analyzer received --canonical-slice,
    # pressure_points.csv carries the slice and pressure_points_full.csv
    # carries everything. When no slice was set, pressure_points.csv is the
    # full file and pressure_points_full.csv is absent. fig_due uses the full
    # file when available so both due_actions are visible.
    pp_path      = os.path.join(args.analysis_dir, "pressure_points.csv")
    pp_full_path = os.path.join(args.analysis_dir, "pressure_points_full.csv")
    pp = _read_csv(pp_path)
    pp_full = _read_csv(pp_full_path) if os.path.exists(pp_full_path) else pp

    per_run   = _read_csv(os.path.join(args.analysis_dir, "per_run_summary.csv"))
    per_kern  = _read_csv(os.path.join(args.analysis_dir, "per_kernel_overhead.csv"))
    per_reg   = _read_csv(os.path.join(args.analysis_dir, "per_region_overhead.csv"))
    per_frame = _read_csv(os.path.join(args.analysis_dir, "per_frame_safety.csv"))
    fm        = _read_csv(os.path.join(args.analysis_dir, "fault_mode_mix.csv"))

    figs_dir = _ensure_dir(os.path.join(args.analysis_dir, "figs"))

    # Case Study B figures (drawn from the sweep CSVs).
    fig_attr(per_region_for_canonical(per_reg, per_frame), figs_dir)
    fig_attr_kernel(per_frame, figs_dir)
    fig_due(pp_full, figs_dir)
    fig_ber_sensitivity(pp, figs_dir, args.unsafe_budget)
    fig_fault_mode_mix(fm, figs_dir)
    fig6_campaign_attribution(per_run, figs_dir)

    # Policy-granularity comparison (placeholder; renderer pending).
    fig_policy(pp, figs_dir)

    # Case Study A figures are produced under --case-studies. Emit
    # placeholder PDFs here so the LaTeX paper compiles even when the
    # case-study artifact has not been generated yet.
    case_study_table = os.path.join(args.analysis_dir,
                                     "case_study_table.csv")
    if not os.path.exists(case_study_table):
        _blank_placeholder(figs_dir, "fig_sanity.pdf",
                           "Case Study A: sanity campaigns C0--C4",
                           "case_study_table.csv not present; "
                           "rerun with run_case_studies.sh + "
                           "make_figures.py --case-studies")
        _blank_placeholder(figs_dir, "fig_pareto.pdf",
                           "Cost--benefit Pareto frontier (C2)",
                           "case_study_table.csv not present; "
                           "rerun with run_case_studies.sh + "
                           "make_figures.py --case-studies")

    table1_headline(pp, args.analysis_dir)

    print(f"Figures + table1 written under {args.analysis_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
