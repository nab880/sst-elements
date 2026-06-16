#!/usr/bin/env python3
# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S.
# Government retains certain rights in this software.
#
# Copyright (c) 2009-2026, NTESS
# All rights reserved.
#
# fft_stats_report.py — tabulate the architectural metrics from one or more
# SST runs of the GPU-driven FFT demo, for side-by-side comparison
# (staged vs shared, N sweep, trace vs sysmode).
#
# Inputs per run:
#   --run LABEL:STATFILE[:STDOUT]
#     STATFILE : an sst.statOutputTXT file (the trace tests' -s output)
#     STDOUT   : optional sst console log, scraped for GPGPU-Sim counters
#
# Host metrics come from the QuetzTestCPU / memHierarchy stats; GPU metrics
# from GPGPU-Sim's text dump in the console log. Missing metrics render as '-'
# so this works whether or not a given run produced them.
#
# Example:
#   fft_stats_report.py --launches 9 \
#       --run staged:staged.stats:staged.out \
#       --run shared:shared.stats:shared.out

import argparse
import re

# Host-side stats (statOutputTXT). Keyed by the stat-name suffix as emitted by
# QuetzTestCPU and memHierarchy; we sum the Sum.u64 field across matching lines.
HOST_STATS = [
    ("cuda_calls_completed", "CUDA round-trips"),
    ("cache_writes", "Scratch cache writes"),
    ("mmio_writes", "MMIO doorbell writes"),
    ("flush_count", "FlushAddr(inv) issued"),
    ("total_memD2H_bytes", "D2H bytes"),
]

# GPGPU-Sim counters scraped from the console log.
#   (key, label, agg) where agg is "last" for cumulative gpu_tot_* counters
#   (GPGPU-Sim reprints the running total each kernel, so the final line is the
#   whole-run value) or "sum" for per-kernel counters that must be added across
#   the 1+log2(N) launches.
GPU_STATS = [
    ("gpu_tot_sim_cycle", "GPU total sim cycles", "last"),
    ("gpu_tot_sim_insn", "GPU total instructions", "last"),
    ("gpu_tot_ipc", "GPU IPC", "last"),
    ("gpgpu_n_mem_read_global", "Global-mem reads", "sum"),
    ("gpgpu_n_mem_write_global", "Global-mem writes", "sum"),
]


def stat_sum(statfile, suffix):
    """Sum the Sum.u64 field of every stat line whose name ends with `.suffix`."""
    if not statfile:
        return None
    total = None
    needle = "." + suffix
    try:
        with open(statfile, "r", errors="replace") as fh:
            for line in fh:
                if needle not in line:
                    continue
                parts = line.replace("=", " ").replace(";", " ").split()
                for i, p in enumerate(parts):
                    if p == "Sum.u64" and i + 1 < len(parts):
                        try:
                            total = (total or 0) + int(parts[i + 1])
                        except ValueError:
                            pass
                        break
    except OSError:
        return None
    return total


def stat_min_f64(statfile, suffix):
    if not statfile:
        return None
    vals = []
    needle = "." + suffix
    try:
        with open(statfile, "r", errors="replace") as fh:
            for line in fh:
                if needle not in line:
                    continue
                parts = line.replace("=", " ").replace(";", " ").split()
                for i, p in enumerate(parts):
                    if p == "Min.f64" and i + 1 < len(parts):
                        try:
                            vals.append(float(parts[i + 1]))
                        except ValueError:
                            pass
                        break
    except OSError:
        return None
    return min(vals) if vals else None


def gpu_scrape(stdoutfile, key, agg="last"):
    """Pull a GPGPU-Sim counter from the log.

    agg="last": the cumulative value (final printed line) — for gpu_tot_*.
    agg="sum" : add every occurrence — for per-kernel counters reprinted each
                launch (e.g. gpgpu_n_mem_read_global).
    """
    if not stdoutfile:
        return None
    pat = re.compile(re.escape(key) + r"\s*=\s*([0-9.eE+-]+)")
    last = None
    total = None
    try:
        with open(stdoutfile, "r", errors="replace") as fh:
            for line in fh:
                m = pat.search(line)
                if not m:
                    continue
                last = m.group(1)
                try:
                    total = (total or 0) + int(m.group(1))
                except ValueError:
                    total = None   # non-integer (e.g. IPC) -> sum not meaningful
    except OSError:
        return None
    return total if (agg == "sum" and total is not None) else last


def fmt(v):
    if v is None:
        return "-"
    if isinstance(v, float):
        return "{:.3f}".format(v)
    return str(v)


def render_table(title, rows, labels):
    width = max([len(title)] + [len(r[0]) for r in rows]) + 2
    colw = max([len(l) for l in labels] + [12]) + 2
    header = title.ljust(width) + "".join(l.rjust(colw) for l in labels)
    print(header)
    print("-" * len(header))
    for name, vals in rows:
        print(name.ljust(width) + "".join(fmt(v).rjust(colw) for v in vals))
    print()


def main():
    ap = argparse.ArgumentParser(description="Tabulate FFT demo SST stats")
    ap.add_argument("--run", action="append", default=[], metavar="LABEL:STATS[:STDOUT]",
                    help="a labeled run; repeat for side-by-side comparison")
    ap.add_argument("--launches", type=int, default=0,
                    help="kernel launches (1+log2 N) for per-launch host cost")
    args = ap.parse_args()

    runs = []
    for spec in args.run:
        parts = spec.split(":")
        label = parts[0]
        statfile = parts[1] if len(parts) > 1 else None
        stdoutfile = parts[2] if len(parts) > 2 else None
        runs.append((label, statfile, stdoutfile))
    if not runs:
        ap.error("provide at least one --run LABEL:STATS[:STDOUT]")

    labels = [r[0] for r in runs]

    host_rows = []
    for suffix, pretty in HOST_STATS:
        host_rows.append((pretty, [stat_sum(s, suffix) for (_, s, _) in runs]))
    # Correctness gate (Min across D2H samples; 1.0 == bit-exact).
    host_rows.append(("D2H min ratio",
                      [stat_min_f64(s, "correct_memD2H_ratio") for (_, s, _) in runs]))
    render_table("HOST <-> GPU command traffic", host_rows, labels)

    if args.launches > 0:
        per_rows = []
        for suffix, pretty in HOST_STATS:
            vals = []
            for (_, s, _) in runs:
                t = stat_sum(s, suffix)
                vals.append(round(t / args.launches, 2) if t is not None else None)
            per_rows.append((pretty, vals))
        render_table("Per kernel-launch (/%d)" % args.launches, per_rows, labels)

    gpu_rows = []
    for key, pretty, agg in GPU_STATS:
        gpu_rows.append((pretty, [gpu_scrape(o, key, agg) for (_, _, o) in runs]))
    render_table("GPGPU-Sim execution", gpu_rows, labels)


if __name__ == "__main__":
    main()
