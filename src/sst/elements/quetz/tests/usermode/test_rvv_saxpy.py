#!/usr/bin/env python3
"""
SST test: RISC-V Vector Extension (RVV) SAXPY via QuetzComponent.

Runs a purpose-built rvv_saxpy binary that computes y = a*x + y on N=64
float32s using RISC-V V-extension intrinsics (RVV 1.0).  A scalar reference
pass confirms the result; the binary prints "PASS" or "FAIL" to stdout.

QEMU user-mode transparently supports the V extension for any binary compiled
with -march=rv64gcv — no extra QEMU flags are needed.

The plugin classifies vector load/store opcodes (0x07/0x27 with vector funct3)
as QEMU_INSN_VEC_MEM so the vector memory traffic appears in a separate
statistic bucket and exec_latency_vec models extra pipeline cycles for the
vector execution unit.

Memory hierarchy: QuetzComponent → L1Cache (32 KiB) → MemController

What the test verifies:
  - Binary prints "PASS: saxpy_rvv correct for N=64".
  - read_requests > 0, write_requests > 0.
  - stall_cycles > 0 (vector latency model engaged by exec_latency_vec=4).

Build the test binary (if rebuilding from source):
  /opt/riscv/bin/riscv64-unknown-linux-musl-gcc \\
    -march=rv64gcv -mabi=lp64d -O2 -static \\
    tests/rvv_saxpy.c -o tests/rvv_saxpy

To run:
  sst test_rvv_saxpy.py
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
exe        = os.path.join(tests_dir, "..", "binaries", "rvv_saxpy")

if not os.path.exists(exe):
    raise FileNotFoundError(
        f"rvv_saxpy not found at {exe}\n"
        "Build with: /opt/riscv/bin/riscv64-unknown-linux-musl-gcc "
        "-march=rv64gcv -mabi=lp64d -O2 -static tests/rvv_saxpy.c "
        "-o tests/rvv_saxpy")

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

    "isa"              : "rv64gcv",
    "has_fpu"          : 1,
    "has_vector"       : 1,
    "vector_vlen"      : 128,
    "vector_elen"      : 64,

    # 4 extra stall cycles for vector load/stores (models a vector pipeline)
    "exec_latency_vec" : 4,
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
l1cache.enableAllStatistics()

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
