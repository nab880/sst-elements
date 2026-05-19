#!/usr/bin/env python3
"""Render publication-grade figures from the CSVs emitted by analyze_ecc_results.py.

Inputs (in <analysis_dir>):
  - pressure_points.csv          (canonical slice if --canonical-slice was set)
  - pressure_points_full.csv     (full factorial; used for the appendix Fig. 5)
  - per_run_summary.csv
  - per_kernel_overhead.csv
  - per_region_overhead.csv
  - per_frame_safety.csv
  - fault_mode_mix.csv

Outputs (under <analysis_dir>/figs/):
  fig1_pressure_point.pdf            Unsafe-action rate vs BER per (scheme, policy),
                                     faceted by scheme, with the unsafe-action budget
                                     drawn as a horizontal reference line.
  fig2_iso_safety_latency.pdf        Mean ECC latency at the iso-safety BER (one BER
                                     per (scheme, policy) chosen by select_iso_ber()).
  fig3a_violation_attribution.pdf    Tier B: safety_violated frames grouped by
                                     attributing_kernel_name (falls back to
                                     kernel_at_close when the simulator hasn't
                                     been rebuilt with attribution yet).
  fig3b_escape_geography.pdf         Tier A (optional): EccGuard escape stack by
                                     (kernel, region). Labeled as escape
                                     classification, NOT violation origin.
  fig4_fault_mode_mix.pdf            Fraction of JEDEC fault modes vs BER.
  appendix_fig5_drop_vs_deadline.pdf Frame-drop vs deadline-miss; uses the FULL
                                     factorial so both due_actions are visible.
  table1_headline.csv                One row per (scheme, policy) on the canonical
                                     slice, with Wilson CIs.

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


def fig1_pressure_point(pp: pd.DataFrame, out_dir: str,
                          unsafe_budget: float) -> None:
    """Unsafe-action rate vs BER per (scheme, policy), faceted by scheme,
    with the safety budget drawn as a horizontal reference line and the
    iso-safety BER (the rightmost row whose Wilson upper bound stays at
    or below the budget) called out per curve.

    Note: pp is expected to be the canonical slice (one fault_model x
    one due_action). The analyzer's --canonical-slice flag enforces this;
    if the caller passed the full factorial we still plot, but curves
    will overlap and the reader cannot tell which fault_model produced
    which point.
    """
    if pp.empty:
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
    fig.savefig(os.path.join(out_dir, "fig1_pressure_point.pdf"))
    plt.close(fig)


def fig2_iso_safety_latency(pp: pd.DataFrame, out_dir: str,
                             unsafe_budget: float) -> None:
    """One bar per (scheme, policy): mean ECC latency at the iso-safety
    BER as defined by select_iso_ber(). The previous implementation
    averaged latency over every BER row whose unsafe rate stayed at or
    below the budget, which (a) over-weighted the low-BER rows where
    latency is dominated by zero correctable events and (b) silently
    fell back to sdc_rate when unsafe_action_rate was missing. Both are
    fixed here: one BER per cell, with a hatched bar when the curve
    never crosses below the budget so the reader can see it as honest
    "no safe BER on this grid".
    """
    if pp.empty:
        return

    iso_table = select_iso_ber_table(pp, unsafe_budget)
    if iso_table.empty:
        return

    df = pp.copy()
    df["ber_f"] = pd.to_numeric(df["ber"], errors="coerce")
    df["mean_ecc_latency_us"] = pd.to_numeric(
        df["mean_ecc_latency_ps"], errors="coerce") / 1e6

    bars: List[Dict[str, object]] = []
    for _, row in iso_table.iterrows():
        scheme, policy, iso_ber = row["scheme"], row["policy"], row["iso_ber"]
        if iso_ber is None or pd.isna(iso_ber):
            bars.append({
                "label":     f"{scheme}/{policy}",
                "mean_us":   0.0,
                "iso_ber":   None,
                "no_cross":  True,
            })
            continue
        match = df[(df["scheme"] == scheme) & (df["policy"] == policy)
                   & (df["ber_f"] == iso_ber)]
        if match.empty:
            bars.append({
                "label":    f"{scheme}/{policy}",
                "mean_us":  0.0,
                "iso_ber":  iso_ber,
                "no_cross": True,
            })
            continue
        bars.append({
            "label":    f"{scheme}/{policy}",
            "mean_us":  float(match.iloc[0]["mean_ecc_latency_us"]),
            "iso_ber":  iso_ber,
            "no_cross": False,
        })

    if not bars:
        return

    fig, ax = plt.subplots(figsize=(6.5, 4.0))
    xs     = list(range(len(bars)))
    labels = [b["label"] for b in bars]
    means  = [b["mean_us"] for b in bars]
    hatches = ["///" if b["no_cross"] else "" for b in bars]
    rects = ax.bar(xs, means)
    for rect, hatch in zip(rects, hatches):
        if hatch:
            rect.set_hatch(hatch)
            rect.set_alpha(0.5)
    # Annotate each bar with its iso-BER (or "no crossing" footnote).
    for x, b in zip(xs, bars):
        if b["no_cross"]:
            ax.text(x, max(means + [0.0]) * 0.05 + 0.001,
                    "no crossing", rotation=90, ha="center", va="bottom",
                    fontsize=7)
        else:
            ax.text(x, b["mean_us"] + max(means) * 0.02,
                    f"BER={b['iso_ber']:.0e}", rotation=0, ha="center",
                    va="bottom", fontsize=7)
    ax.set_xticks(xs)
    ax.set_xticklabels(labels, rotation=30, ha="right")
    ax.set_ylabel("Mean ECC latency overhead (us / event)")
    ax.set_title(f"Latency at iso-safety BER (unsafe_action_rate <= {unsafe_budget:g})")
    ax.grid(True, axis="y", linestyle=":", alpha=0.5)
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "fig2_iso_safety_latency.pdf"))
    plt.close(fig)


def fig3a_violation_attribution(per_frame: pd.DataFrame,
                                  out_dir: str,
                                  canonical_fm: str = "jedec_mix",
                                  canonical_due: str = "drop_frame") -> None:
    """Tier B: among scorer-flagged safety_violated == 1 frames, group by
    attributing_kernel_name (the kernel that produced the most escapes in
    that frame; populated by the simulator). Falls back to kernel_name
    (== ACTUATE for every frame on the legacy build) so old artifacts
    still render -- but the caption needs to call out the fallback when
    only one bar is visible.

    Filters to the canonical (fault_model, due_action) slice to avoid
    mixing non-canonical axes into the attribution bars.
    """
    if per_frame.empty:
        return
    df = per_frame.copy()
    if "fault_model" in df.columns and "due_action" in df.columns:
        df = df[(df["fault_model"] == canonical_fm) & (df["due_action"] == canonical_due)]
    df = df[df["safety_violated"] == 1]
    if df.empty:
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
    ax.set_title("Fig. 3a Violation attribution by VLA kernel\n" + title_suffix)
    plt.setp(ax.get_xticklabels(), rotation=35, ha="right")
    ax.grid(True, axis="y", linestyle=":", alpha=0.5)
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "fig3a_violation_attribution.pdf"))
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


def fig3b_escape_geography(per_region: pd.DataFrame, out_dir: str) -> None:
    """Tier A (optional): EccGuard escape stack by (kernel_name, region).

    The caption MUST label this as "EccGuard escape classification, NOT
    safety-violation origin". An escape on a memory response and a frame
    flagged safety_violated by the scorer are two different layers
    (DRAM/ECC layer vs behavioral layer); see paper Section 3 / appendix
    glossary.
    """
    if per_region.empty:
        return
    df = per_region.copy()
    if "escape" not in df.columns:
        return
    df["escape"] = pd.to_numeric(df["escape"], errors="coerce").fillna(0)
    df = df[df["escape"] > 0]
    if df.empty:
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
    ax.set_title("Fig. 3b EccGuard escape geography by (kernel, region)\n"
                 "(NOT violation origin; see caption / appendix glossary)")
    plt.setp(ax.get_xticklabels(), rotation=40, ha="right", fontsize=8)
    ax.grid(True, axis="y", linestyle=":", alpha=0.5)
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "fig3b_escape_geography.pdf"))
    plt.close(fig)


def fig4_fault_mode_mix(fm: pd.DataFrame, out_dir: str) -> None:
    if fm.empty:
        return
    df = fm[fm["fault_model"] == "jedec_mix"].copy()
    if df.empty:
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
    fig.savefig(os.path.join(out_dir, "fig4_fault_mode_mix.pdf"))
    plt.close(fig)


def appendix_fig5_drop_vs_deadline(pp_full: pd.DataFrame, out_dir: str) -> None:
    """Appendix-only: frame-drop vs deadline-miss split by due_action.
    HEADLINE locks the main-text axes to drop_frame, so this figure exists
    only when the FULL_CUBE artifact (or a custom run) carries both
    `latency_only` and `drop_frame`. Empty input -> no figure.
    """
    if pp_full.empty:
        return
    df = pp_full.copy()
    df["ber_f"] = pd.to_numeric(df["ber"], errors="coerce")
    df["drop_rate"] = pd.to_numeric(df["drop_rate"], errors="coerce").fillna(0.0)
    df["deadline_viol_rate"] = pd.to_numeric(df["deadline_viol_rate"], errors="coerce")
    df = df.dropna(subset=["ber_f"])
    if df.empty:
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
    ax.set_title("Appendix Fig. 5 Frame drop vs deadline-miss by due_action\n"
                 "(supplement only; HEADLINE main text uses drop_frame only)")
    ax.grid(True, which="both", linestyle=":", alpha=0.5)
    ax.legend(fontsize=7, loc="best")
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "appendix_fig5_drop_vs_deadline.pdf"))
    plt.close(fig)


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

    # Canonical-slice file layout: when the analyzer received --canonical-slice,
    # pressure_points.csv carries the slice and pressure_points_full.csv carries
    # everything. When no slice was set, pressure_points.csv is the full file
    # and pressure_points_full.csv is absent. Fig. 5 uses the full file when
    # available so both due_actions are visible in the appendix figure.
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

    fig1_pressure_point(pp, figs_dir, args.unsafe_budget)
    fig2_iso_safety_latency(pp, figs_dir, args.unsafe_budget)
    fig3a_violation_attribution(per_frame, figs_dir)
    fig3b_escape_geography(per_region_for_canonical(per_reg, per_frame),
                           figs_dir)
    fig4_fault_mode_mix(fm, figs_dir)
    appendix_fig5_drop_vs_deadline(pp_full, figs_dir)
    fig6_campaign_attribution(per_run, figs_dir)
    table1_headline(pp, args.analysis_dir)

    print(f"Figures + table1 written under {args.analysis_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
