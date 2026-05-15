#!/usr/bin/env python3
"""
SST test: x86-64 hello/pi binary via QuetzComponent.

Runs the pre-compiled x86-64 binary (hello_x86_64) built from hello_multiisa.c
under qemu-x86_64.  x86 instruction encoding is complex to decode at
TB-translation time, so all accesses are handled by the generic size-based
fallback (accesses >= 16 bytes → VEC_MEM, else INT_MEM).  This verifies that
the GENERIC ISA path works end-to-end on the most common host architecture.

Memory hierarchy: QuetzComponent → L1 (32 KiB, MSI) → MemController

Expected output: "pi ~ 3.141493"

What the test verifies:
  - qemu-x86_64 attaches to the SST shared-memory tunnel.
  - Plugin reports ISA class "generic".
  - Binary runs to completion.
  - read_requests > 0, write_requests > 0.

To run:
  sst test_x86_hello.py
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

if not os.path.exists(qemu_bin):
    raise FileNotFoundError(
        f"qemu-x86_64 not found at {qemu_bin}. "
        "Build QEMU with --target-list=x86_64-linux-user.")

exe = os.path.join(tests_dir, "..", "binaries", "hello_x86_64")
if not os.path.exists(exe):
    raise FileNotFoundError(
        f"hello_x86_64 not found at {exe}. "
        "Build it with: gcc -O2 -static -o hello_x86_64 hello_multiisa.c")

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
    "isa"              : "x86_64",
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
mem_be.addParams({"access_time": "100ns", "mem_size": "256TiB"})

# ---------------------------------------------------------------------------
# Links
# ---------------------------------------------------------------------------
sst.Link("cpu_l1").connect((cpu,     "cache_link_0", "1ns"),
                            (l1cache, "highlink",     "1ns"))
sst.Link("l1_mem").connect( (l1cache, "lowlink",  "50ns"),
                             (memctrl, "highlink", "50ns"))

sst.setProgramOption("timebase", "1ps")
sst.setStatisticLoadLevel(4)
sst.setStatisticOutput("sst.statOutputConsole")
