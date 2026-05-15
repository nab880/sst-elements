#!/usr/bin/env python3
"""
SST test: MIPS EL (mipsel) hello-world via QuetzComponent.

Runs the pre-compiled MIPS 32-bit little-endian hello-world binary from
the vanadis test suite under qemu-mipsel.  The MIPS instruction encoding
is not natively classified by the plugin; all accesses are handled by
the generic size-based fallback (accesses >= 16 bytes → VEC_MEM, else
INT_MEM).  This verifies that the GENERIC ISA path works end-to-end and
that the component correctly handles a non-RISC-V target.

Memory hierarchy: QuetzComponent → L1 (32 KiB, MSI) → MemController

Expected output: "Hello World from Vanadis"

What the test verifies:
  - qemu-mipsel attaches to the SST shared-memory tunnel.
  - Plugin reports ISA class "generic".
  - Binary runs to completion.
  - read_requests > 0, write_requests > 0.

To run:
  sst test_mips_hello.py
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
qemu_bin   = os.path.join(sst_home, "bin", "qemu-mipsel")
tests_dir  = os.path.dirname(os.path.abspath(__file__))

if not os.path.exists(qemu_bin):
    raise FileNotFoundError(
        f"qemu-mipsel not found at {qemu_bin}. "
        "Build QEMU with --target-list=mipsel-linux-user.")

# Pre-compiled MIPS hello-world from the vanadis test suite.
exe = os.path.join(tests_dir, "..", "..", "..", "vanadis", "tests", "small",
                   "basic-io", "hello-world", "mipsel", "hello-world")
exe = os.path.normpath(exe)
if not os.path.exists(exe):
    raise FileNotFoundError(f"MIPS hello-world not found at {exe}")

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
    "isa"              : "mipsel",
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
