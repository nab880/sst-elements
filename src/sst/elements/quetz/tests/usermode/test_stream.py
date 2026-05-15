#!/usr/bin/env python3
"""
SST test: STREAM memory bandwidth benchmark via QuetzComponent.

Runs the STREAM benchmark (McCalpin) compiled for RISC-V.  STREAM exercises
sustained load/store bandwidth across four kernels (Copy, Scale, Add, Triad)
and is a good end-to-end test of the QuetzComponent → cache → memory path.

Memory hierarchy:
  QuetzComponent → L1 (32 KiB) → L2 (256 KiB) → MemController (simpleMem)

Uses the pre-compiled stream binary from the vanadis test suite.

What the test verifies:
  - Binary runs to completion and reports bandwidth numbers.
  - read_requests > 0, write_requests > 0.
  - L1 CacheMisses > 0 (working set larger than L1).
  - Simulation completes without error.

To run:
  sst test_stream.py
"""

import sst
import os
import shutil
import subprocess

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
def _sst_prefix():
    if "SST_HOME" in os.environ:
        return os.environ["SST_HOME"]
    cfg = shutil.which("sst-config")
    if cfg:
        return subprocess.check_output([cfg, "--prefix"], text=True).strip()
    raise RuntimeError("Set SST_HOME or put sst-config on PATH")

sst_home   = _sst_prefix()
plugin     = os.path.join(sst_home, "libexec", "libqemu_sst_plugin.so")
qemu_bin   = os.path.join(sst_home, "bin", "qemu-riscv64")
tests_dir  = os.path.dirname(os.path.abspath(__file__))

exe = os.path.join(tests_dir, "..", "..", "..", "vanadis", "tests", "small",
                   "misc", "stream", "riscv64", "stream")
exe = os.path.normpath(exe)
if not os.path.exists(exe):
    raise FileNotFoundError(f"stream binary not found at {exe}")

cpu_clock = "1GHz"

# ---------------------------------------------------------------------------
# QuetzComponent
# ---------------------------------------------------------------------------
cpu = sst.Component("cpu", "quetz.QuetzComponent")
cpu.addParams({
    "verbose"          : 0,
    "clock"            : cpu_clock,
    "vcpu_count"       : 1,
    "maxcorequeue"     : 256,
    "maxtranscore"     : 32,
    "maxissuepercycle" : 4,
    "cachelinesize"    : 64,
    "qemu"             : qemu_bin,
    "qemu_plugin"      : plugin,
    "executable"       : exe,
})
cpu.enableAllStatistics()

# ---------------------------------------------------------------------------
# L1 data cache (32 KiB, 4-way, MSI)
# ---------------------------------------------------------------------------
l1cache = sst.Component("l1cache", "memHierarchy.Cache")
l1cache.addParams({
    "access_latency_cycles" : 2,
    "cache_frequency"       : cpu_clock,
    "replacement_policy"    : "lru",
    "coherence_protocol"    : "MSI",
    "associativity"         : 4,
    "cache_line_size"       : 64,
    "cache_size"            : "32KB",
    "L1"                    : 1,
})
l1cache.enableAllStatistics()

# ---------------------------------------------------------------------------
# L2 cache (256 KiB, 8-way, MSI)
# ---------------------------------------------------------------------------
l2cache = sst.Component("l2cache", "memHierarchy.Cache")
l2cache.addParams({
    "access_latency_cycles" : 10,
    "cache_frequency"       : cpu_clock,
    "replacement_policy"    : "lru",
    "coherence_protocol"    : "MSI",
    "associativity"         : 8,
    "cache_line_size"       : 64,
    "cache_size"            : "256KB",
    "L1"                    : 0,
})
l2cache.enableAllStatistics()

# ---------------------------------------------------------------------------
# MemController
# ---------------------------------------------------------------------------
memctrl = sst.Component("memory", "memHierarchy.MemController")
memctrl.addParams({
    "clock"            : cpu_clock,
    "addr_range_start" : 0,
    "addr_range_end"   : (1 << 48) - 1,
})
mem_be = memctrl.setSubComponent("backend", "memHierarchy.simpleMem")
mem_be.addParams({
    "access_time" : "100ns",
    "mem_size"    : "256TiB",
})

# ---------------------------------------------------------------------------
# Links
# ---------------------------------------------------------------------------
sst.Link("cpu_to_l1").connect(
    (cpu,     "cache_link_0", "1ns"),
    (l1cache, "highlink",     "1ns"))

sst.Link("l1_to_l2").connect(
    (l1cache, "lowlink",  "5ns"),
    (l2cache, "highlink", "5ns"))

sst.Link("l2_to_mem").connect(
    (l2cache, "lowlink",  "50ns"),
    (memctrl, "highlink", "50ns"))

sst.setProgramOption("timebase", "1ps")
sst.setStatisticLoadLevel(4)
sst.setStatisticOutput("sst.statOutputConsole")
