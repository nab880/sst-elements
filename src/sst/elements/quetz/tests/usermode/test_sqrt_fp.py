#!/usr/bin/env python3
"""
SST test: scalar FP instruction class tracking via sqrt-float.

Runs the vanadis pre-compiled sqrt-float benchmark under QuetzComponent.
The binary uses RISC-V scalar FP instructions (fld/fsw, fsqrt.d) so
filtered_reads and filtered_writes will be zero and read_requests > 0.

Demonstrates exec_latency_fp: setting it to 2 adds two extra stall cycles
per scalar FP load/store before the request reaches the cache.  The effect
is visible as stall_cycles > 0 in the statistics output.

What the test verifies:
  - Binary runs to completion and prints sqrt results.
  - read_requests > 0 (FP loads forwarded to cache).
  - stall_cycles > 0 (FP latency model engaged).

To run:
  sst test_sqrt_fp.py
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
                   "basic-math", "sqrt-float", "riscv64", "sqrt-float")
exe = os.path.normpath(exe)
if not os.path.exists(exe):
    raise FileNotFoundError(f"sqrt-float binary not found at {exe}")

# ---------------------------------------------------------------------------
# QuetzComponent
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

    # ISA description (informational)
    "isa"              : "rv64gfd",
    "has_fpu"          : 1,

    # 2 extra stall cycles before a scalar FP load/store issues to cache
    "exec_latency_fp"  : 2,
})
cpu.enableAllStatistics()

# ---------------------------------------------------------------------------
# L1 cache + MemController
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
