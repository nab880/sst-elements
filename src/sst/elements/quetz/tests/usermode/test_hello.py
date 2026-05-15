#!/usr/bin/env python3
"""
SST test: QuetzComponent smoke test — hello-world with a direct MemController.

Memory hierarchy: QuetzComponent → MemController (simpleMem, no cache).

Uses the pre-compiled hello-world binary from the vanadis test suite.
Expected output on stdout: "Hello World from Vanadis"

What the test verifies:
  - Component initialises and QEMU attaches.
  - The binary runs to completion.
  - read_requests > 0, write_requests > 0.

To run:
  sst test_hello.py
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

# Pre-compiled RISC-V hello-world from the vanadis test suite.
exe = os.path.join(tests_dir, "..", "..", "..", "vanadis", "tests", "small",
                   "basic-io", "hello-world", "riscv64", "hello-world")
exe = os.path.normpath(exe)
if not os.path.exists(exe):
    raise FileNotFoundError(f"hello-world binary not found at {exe}")

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
})
cpu.enableAllStatistics()

# ---------------------------------------------------------------------------
# MemController (no cache, wide address range)
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
# Link
# ---------------------------------------------------------------------------
sst.Link("cpu_to_mem").connect(
    (cpu,     "cache_link_0", "1ns"),
    (memctrl, "highlink",     "1ns"))

sst.setProgramOption("timebase", "1ps")
sst.setStatisticLoadLevel(4)
sst.setStatisticOutput("sst.statOutputConsole")
