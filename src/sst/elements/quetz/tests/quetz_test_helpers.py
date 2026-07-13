# -*- coding: utf-8 -*-
"""
quetz_test_helpers.py — shared fixtures for Quetz SST testsuites.
"""

import os

try:
    from sst_unittest_support import LineFilter
except ImportError:
    # SDL decks import this module (for sst_prefix) under a plain `sst` process,
    # where the test-framework module is not importable (it installs to
    # $SST_PREFIX/libexec, which is not on the default path). LineFilter — and the
    # QuetzStatsFilter subclass below — are only exercised by the testsuite, which
    # runs under sst-test-elements where the real import succeeds.
    LineFilter = object


def sst_prefix():
    """Resolve the SST install prefix from SST_HOME or sst-config.

    The test harness exports SST_HOME for every deck it launches; manual
    `sst <deck>.py` runs fall back to sst-config on PATH. SDL decks import this
    after inserting the tests/ directory on sys.path.
    """
    home = os.environ.get("SST_HOME", "")
    if home:
        return home
    import shutil
    import subprocess
    cfg = shutil.which("sst-config")
    if cfg:
        return subprocess.check_output([cfg, "--prefix"], text=True).strip()
    raise RuntimeError("Set SST_HOME or put sst-config on PATH")

# Timing-sensitive or environment-variable stats excluded from gold diffs:
#   _latency          — read/write/mmio latency accumulators
#   .cycles.          — cycle counters (e.g. cpu.cycles.0)
#   active_cycles     — cycles with memory activity
#   stall_cycles      — exec/compute stall accumulators
#   instruction_count — includes QEMU startup/shutdown NOPs
#   no_ops            — idle NOP cycles, varies across environments
_TIMING_KEYWORDS = (
    "_latency",
    ".cycles.",
    "active_cycles",
    "stall_cycles",
    "instruction_count",
    "no_ops",
)

# Round 2 Balar doorbell stats: zero in non-Balar tests and absent from pre-r2 gold.
# cached_truncated_writes: zero unless a >64B store is split; absent from older gold.
# async_*: P4 async-offload stats, zero in non-async tests and absent from pre-P4 gold.
_GOLD_EXCLUDE_KEYWORDS = (
    "mmio_doorbell_flushes",
    "mmio_doorbell_flush_cycles",
    "cached_truncated_writes",
    "async_submits",
    "async_completions",
    "async_overlap_cycles",
)


class QuetzStatsFilter(LineFilter):
    """Keep only deterministic event-count statistics from QuetzComponent."""

    def filter(self, line):
        if not line.startswith(" cpu."):
            return None
        for kw in _TIMING_KEYWORDS + _GOLD_EXCLUDE_KEYWORDS:
            if kw in line:
                return None
        return line


def should_compare_gold():
    """False when cross-stack CI skips stat gold (see QUETZ_SKIP_GOLD)."""
    return os.getenv("QUETZ_SKIP_GOLD", "0") != "1"


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


def make_serial_feed(name, data_file, hz=0.0):
    """FIFO-backed `-serial pipe:` feed for a dedicated guest serial port.

    Creates <base>.in/.out FIFOs in a fresh local tmpdir (NOT the test outdir:
    FIFOs are unreliable on bind-mounted trees under Docker Desktop), starts
    tools/serial_feeder.py writing data_file into it (line-paced at `hz`), and
    returns (serial_arg, cleanup) where serial_arg is the fragment to append
    to QUETZ_QEMU_ARGS (e.g. "-serial pipe:/tmp/qz_serial_x/gps") and
    cleanup() stops the feeder and removes the FIFOs — call it in a finally.
    """
    import shutil
    import subprocess
    import sys as _sys
    import tempfile

    tmpdir = tempfile.mkdtemp(prefix="qz_serial_")
    base = os.path.join(tmpdir, name)
    os.mkfifo(base + ".in")
    os.mkfifo(base + ".out")

    feeder = os.path.normpath(os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "tools",
        "serial_feeder.py"))
    cmd = [_sys.executable, feeder, data_file, base + ".in"]
    if hz:
        cmd += ["--hz", str(hz)]
    proc = subprocess.Popen(cmd)

    def cleanup():
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(tmpdir, ignore_errors=True)

    return "-serial pipe:" + base, cleanup


def enable_mmio_payload_delivery():
    """Enable QEMU bridge / linux-user MMIO sync (launcher reads these)."""
    os.environ["QUETZ_MMIO_PAYLOAD"] = "1"


def assert_sink_equals(path, expected_bytes, context=""):
    """Byte-exact comparison of a QuetzSinkDevice capture file.

    Raises AssertionError with the first differing offset (and a short hex
    excerpt of both sides) so a capture mismatch is diagnosable from CI logs.
    """
    with open(path, "rb") as f:
        got = f.read()
    if got == expected_bytes:
        return
    prefix = "sink capture {} ".format(context).rstrip() + ": "
    if len(got) != len(expected_bytes):
        raise AssertionError(
            prefix + "length {} != expected {} ({})".format(
                len(got), len(expected_bytes), path))
    for i, (g, e) in enumerate(zip(got, expected_bytes)):
        if g != e:
            lo, hi = max(0, i - 4), i + 4
            raise AssertionError(
                prefix + "first mismatch at offset {}: got {} expected {} "
                "({})".format(i, got[lo:hi].hex(), expected_bytes[lo:hi].hex(),
                              path))


def make_usermode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                      with_l1=False, isa="", detailed=True):
    """Populate os.environ for basic_quetz.py usermode runs."""
    os.environ.pop("QUETZ_MMIO_PAYLOAD", None)
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


def _clear_region_handler_env():
    """Remove stale QUETZ_REGION_HANDLER* keys from prior tests in this process."""
    for key in [k for k in os.environ if k.startswith("QUETZ_REGION_HANDLER")]:
        del os.environ[key]


def apply_usermode_region_handlers(memmaps):
    """Set QUETZ_REGION_HANDLER{n}_* for usermode GPU SDLs (slots 1+ in SDL)."""
    _clear_region_handler_env()
    os.environ["QUETZ_REGION_HANDLER_COUNT"] = str(len(memmaps))
    for n, entry in enumerate(memmaps):
        if len(entry) >= 5:
            _name, start, end, rtype, extras = (
                entry[0], entry[1], entry[2], entry[3], entry[4])
        else:
            _name, start, end, rtype = entry[0], entry[1], entry[2], entry[3]
            extras = None
        os.environ[f"QUETZ_REGION_HANDLER{n}_START"] = str(start)
        os.environ[f"QUETZ_REGION_HANDLER{n}_END"] = str(end)
        os.environ[f"QUETZ_REGION_HANDLER{n}_TYPE"] = rtype
        if extras:
            for key, val in extras.items():
                os.environ[f"QUETZ_REGION_HANDLER{n}_{key.upper()}"] = str(val)


def make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                     qemu_args, loader, ram_start, ram_end, memmaps,
                     stdin_file="", stdout_file="", platform=""):
    """Populate os.environ for basic_quetz_sysmode.py runs."""
    _clear_region_handler_env()
    os.environ.pop("QUETZ_MMIO_PAYLOAD", None)
    # SST-backed window (second bridge aperture): only device-compute tests set
    # these; stale values would map the aperture into every later run.
    os.environ.pop("QUETZ_SST_WIN_START", None)
    os.environ.pop("QUETZ_SST_WIN_END", None)
    os.environ.pop("QUETZ_WIN_BIG_ENDIAN", None)
    os.environ.pop("QUETZ_BSP_PROFILE", None)
    os.environ.pop("QUETZ_BSP_DISCOVER", None)
    os.environ.pop("QUETZ_BSP_TARGET", None)
    os.environ.pop("QUETZ_BSP_LOG", None)
    os.environ.pop("QUETZ_IRQ_POLL_NS", None)
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
    for n, entry in enumerate(memmaps):
        if len(entry) >= 5:
            _name, start, end, rtype, extras = entry[0], entry[1], entry[2], entry[3], entry[4]
        else:
            _name, start, end, rtype = entry[0], entry[1], entry[2], entry[3]
            extras = None
        os.environ[f"QUETZ_REGION_HANDLER{n}_START"] = str(start)
        os.environ[f"QUETZ_REGION_HANDLER{n}_END"] = str(end)
        os.environ[f"QUETZ_REGION_HANDLER{n}_TYPE"] = rtype
        if extras:
            for key, val in extras.items():
                os.environ[f"QUETZ_REGION_HANDLER{n}_{key.upper()}"] = str(val)


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


def _stat_sum_tolerance(stat_name, value):
    """Allow run-to-run drift in event-count statistics (QEMU/plugin jitter)."""
    if "request_sizes" in stat_name or stat_name.startswith("cpu.split_"):
        return max(2000, value // 20)
    return max(500, value // 200)


def _filtered_stats_within_tolerance(outfile, reffile):
    """True when deterministic stat lines match within per-stat tolerance."""
    out_stats = {}
    for line in filtered_stat_lines(outfile):
        name = line.split(" : ")[0].strip()
        val = int(line.split("Sum.u64 = ")[1].split(";")[0])
        out_stats[name] = val

    ref_stats = {}
    for line in filtered_stat_lines(reffile):
        name = line.split(" : ")[0].strip()
        val = int(line.split("Sum.u64 = ")[1].split(";")[0])
        ref_stats[name] = val

    if set(out_stats) != set(ref_stats):
        return False

    for name in out_stats:
        delta = abs(out_stats[name] - ref_stats[name])
        limit = _stat_sum_tolerance(name, max(out_stats[name], ref_stats[name]))
        if delta > limit:
            return False
    return True


def compare_gold(testname, sst_outfile, ref_outfile, update_files=False):
    """Diff sst_outfile against ref_outfile; optionally refresh gold."""
    from sst_unittest_support import (
        testing_compare_filtered_diff,
        testing_get_diff_data,
        log_failure,
    )

    cmp_result = testing_compare_filtered_diff(
        testname, sst_outfile, ref_outfile, filters=[QuetzStatsFilter()])
    if cmp_result:
        return True

    if update_files:
        import subprocess
        print("Updating gold file", sst_outfile, "->", ref_outfile)
        subprocess.call(["cp", sst_outfile, ref_outfile])
        return True

    if _filtered_stats_within_tolerance(sst_outfile, ref_outfile):
        return True

    diffdata = testing_get_diff_data(testname)
    log_failure(diffdata)
    return False
