# -*- coding: utf-8 -*-
"""
quetz_test_helpers.py — shared fixtures for Quetz SST testsuites.
"""

import os

from sst_unittest_support import LineFilter

# instruction_count and no_ops are excluded from gold diffs: they include idle
# NOP cycles from QEMU startup/shutdown and vary across environments.
_TIMING_KEYWORDS = (
    "_latency",
    ".cycles.",
    "active_cycles",
    "stall_cycles",
    "instruction_count",
    "no_ops",
)


class QuetzStatsFilter(LineFilter):
    """Keep only deterministic event-count statistics from QuetzComponent."""

    def filter(self, line):
        if not line.startswith(" cpu."):
            return None
        for kw in _TIMING_KEYWORDS:
            if kw in line:
                return None
        return line


def parse_stats(outfile):
    """Parse SST console stat lines into {name: Sum.u64 value}."""
    stats = {}
    with open(outfile, "r") as f:
        for line in f:
            if "Sum.u64 = " in line and "cpu." in line:
                name = line.strip().split(" : ")[0].strip()
                val = int(line.split("Sum.u64 = ")[1].split(";")[0])
                stats[name] = val
    return stats


def stat_sum(output, stat_substr, default=None):
    """Return Sum.u64 for the first line containing stat_substr, or default."""
    for line in output.splitlines():
        if stat_substr in line and "Sum.u64 =" in line:
            return int(line.split("Sum.u64 = ")[1].split(";")[0])
    return default


def make_usermode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                      with_l1=False, isa="", detailed=True):
    """Populate os.environ for basic_quetz.py usermode runs."""
    env = {
        "QUETZ_EXE": exe_abs,
        "QUETZ_QEMU": qemu_bin,
        "QUETZ_PLUGIN": os.path.join(sst_libexec, "libqemu_sst_plugin.so"),
        "QUETZ_WITH_L1": "1" if with_l1 else "0",
        "QUETZ_ISA": isa,
        "QUETZ_DETAILED": "1" if detailed else "0",
        "SST_HOME": sst_prefix,
    }
    for k, v in env.items():
        os.environ[k] = v
    return env


def make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                     qemu_args, loader, ram_start, ram_end, memmaps,
                     stdin_file="", stdout_file="", platform=""):
    """Populate os.environ for basic_quetz_sysmode.py runs."""
    os.environ["QUETZ_EXE"] = exe_abs
    os.environ["QUETZ_QEMU"] = qemu_bin
    os.environ["QUETZ_PLUGIN"] = os.path.join(sst_libexec, "libqemu_sst_plugin.so")
    os.environ["QUETZ_QEMU_ARGS"] = qemu_args
    os.environ["QUETZ_LOADER"] = loader
    os.environ["QUETZ_RAM_START"] = str(ram_start)
    os.environ["QUETZ_RAM_END"] = str(ram_end)
    os.environ["QUETZ_REGION_HANDLER_COUNT"] = str(len(memmaps))
    os.environ["SST_HOME"] = sst_prefix
    os.environ["QUETZ_STDIN_FILE"] = stdin_file
    os.environ["QUETZ_STDOUT_FILE"] = stdout_file
    if platform:
        os.environ["QUETZ_PLATFORM"] = platform
    for n, (_name, start, end, rtype) in enumerate(memmaps):
        os.environ[f"QUETZ_REGION_HANDLER{n}_START"] = str(start)
        os.environ[f"QUETZ_REGION_HANDLER{n}_END"] = str(end)
        os.environ[f"QUETZ_REGION_HANDLER{n}_TYPE"] = rtype


def filtered_stat_lines(outfile):
    """Return sorted deterministic stat lines from an SST stdout file."""
    filt = QuetzStatsFilter()
    lines = []
    with open(outfile, "r") as f:
        for line in f:
            kept = filt.filter(line)
            if kept is not None:
                lines.append(kept.rstrip("\n"))
    return lines


def assert_class_balance(stats, core_id=0):
    """Verify per-class instruction counters sum to instruction_count."""
    cid = str(core_id)
    needed = [
        "cpu.read_requests." + cid,
        "cpu.write_requests." + cid,
        "cpu.int_compute." + cid,
        "cpu.fp_compute." + cid,
        "cpu.vec_compute." + cid,
        "cpu.branch." + cid,
        "cpu.instruction_count." + cid,
        "cpu.no_ops." + cid,
    ]
    for s in needed:
        if s not in stats:
            raise AssertionError("Statistic {} not found in output".format(s))

    classified_nop = (stats["cpu.int_compute." + cid] +
                      stats["cpu.fp_compute." + cid] +
                      stats["cpu.vec_compute." + cid] +
                      stats["cpu.branch." + cid])
    other_nop = stats["cpu.no_ops." + cid] - classified_nop
    class_sum = (stats["cpu.read_requests." + cid] +
                 stats["cpu.write_requests." + cid] +
                 classified_nop + other_nop)
    insn_count = stats["cpu.instruction_count." + cid]
    delta = abs(class_sum - insn_count)
    limit = max(100, insn_count // 500)
    if delta > limit:
        raise AssertionError(
            "Class-balance identity failed: sum {} != instruction_count {} "
            "(delta {}, limit {})".format(class_sum, insn_count, delta, limit))


def compare_gold(testname, sst_outfile, ref_outfile, update_files=False):
    """Diff sst_outfile against ref_outfile; optionally refresh gold."""
    from sst_unittest_support import (
        testing_compare_filtered_diff,
        testing_get_diff_data,
        log_failure,
    )

    cmp_result = testing_compare_filtered_diff(
        testname, sst_outfile, ref_outfile, filters=[QuetzStatsFilter()])
    if not cmp_result:
        diffdata = testing_get_diff_data(testname)
        log_failure(diffdata)
        if update_files:
            import subprocess
            print("Updating gold file", sst_outfile, "->", ref_outfile)
            subprocess.call(["cp", sst_outfile, ref_outfile])
    return cmp_result
