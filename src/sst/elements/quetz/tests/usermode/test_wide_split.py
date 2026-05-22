#!/usr/bin/env python3
"""
SST test: verify cache-line splitting in QuetzCore::issueRead / issueWrite.

Runs the bundled x86_64 hello binary under QEMU user-mode.  Guest code and
libc startup issue some 16-byte loads/stores that straddle 64-byte cache
lines; the plugin forwards each access width to QuetzCore, which walks the
ceil(size/line) loop and records extra sub-requests in split_* statistics.

(RISC-V vector loads are micro-op'd to 4-byte plugin callbacks in QEMU 9.x,
so rvv_saxpy does not exercise multi-line splits — x86 is used here instead.)

Memory hierarchy: QuetzComponent -> L1Cache (32 KiB) -> MemController

What the test verifies:
  - split_read_requests.0 > 0
  - Simulation completes without error.

To run:
  sst test_wide_split.py
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
qemu_bin   = os.path.join(sst_home, "bin", "qemu-x86_64")
tests_dir  = os.path.dirname(os.path.abspath(__file__))
exe        = os.path.join(tests_dir, "..", "binaries", "hello_x86_64")

if not os.path.exists(exe):
    raise FileNotFoundError(f"hello_x86_64 not found at {exe}")

# ---------------------------------------------------------------------------
# QuetzComponent — x86 guest with line-crossing SIMD/mem ops
# ---------------------------------------------------------------------------
cpu = sst.Component("cpu", "quetz.QuetzComponent")
cpu.addParams({
    "verbose"          : 1,
    "clock"            : "1GHz",
    "vcpu_count"       : 1,
    "maxcorequeue"     : 64,
    "maxtranscore"     : 16,
    "maxissuepercycle" : 2,
    "cachelinesize"    : 64,
    "qemu"             : qemu_bin,
    "qemu_plugin"      : plugin,
    "executable"       : exe,

    "isa"              : "x86_64",
    "has_fpu"          : 1,
    "checkaddresses"   : 1,
})
cpu.enableAllStatistics()

# ---------------------------------------------------------------------------
# L1 cache (32 KiB, 4-way, MSI)
# ---------------------------------------------------------------------------
l1cache = sst.Component("l1cache", "memHierarchy.Cache")
l1cache.addParams({
    "access_latency_cycles" : 2,
    "cache_frequency"       : "1GHz",
    "replacement_policy"    : "lru",
    "coherence_protocol"    : "MSI",
    "associativity"         : 4,
    "cache_line_size"       : 64,
    "cache_size"            : "32KB",
    "L1"                    : 1,
})

# ---------------------------------------------------------------------------
# MemController
# ---------------------------------------------------------------------------
memctrl = sst.Component("memory", "memHierarchy.MemController")
memctrl.addParams({
    "clock"            : "1GHz",
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

sst.Link("l1_to_mem").connect(
    (l1cache, "lowlink",  "50ns"),
    (memctrl, "highlink", "50ns"))

sst.setProgramOption("timebase", "1ps")
sst.setStatisticLoadLevel(4)
sst.setStatisticOutput("sst.statOutputConsole")
