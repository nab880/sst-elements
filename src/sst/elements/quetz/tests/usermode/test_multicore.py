#!/usr/bin/env python3
"""
SST test: multi-vCPU simulation with OpenMP hello-world.

Runs the vanadis openmp binary with 2 threads under QuetzComponent.  QEMU
user-mode creates a new vCPU entry in the plugin for each pthread, so the
shared-memory tunnel carries traffic from both threads.  QuetzComponent
allocates one QuetzCore + one cache_link per vCPU.

Memory hierarchy (each vCPU has its own L1; both L1s share a bus):
  QuetzComponent (vcpu_count=2)
    cache_link_0 → L1_0 (32 KiB) ─┐
    cache_link_1 → L1_1 (32 KiB) ─┴─ Bus → MemController

What the test verifies:
  - Binary runs to completion and prints "Number of threads = 2".
  - Simulation completes without error.
  - read_requests > 0, write_requests > 0 on each vCPU.

Note: the openmp binary uses OMP_NUM_THREADS to set its thread count.
We pass it via envparamcount so the child process inherits it correctly.

To run:
  sst test_multicore.py
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
                   "misc", "openmp", "riscv64", "openmp")
exe = os.path.normpath(exe)
if not os.path.exists(exe):
    raise FileNotFoundError(f"openmp binary not found at {exe}")

cpu_clock  = "1GHz"
NUM_VCPUS  = 2

# ---------------------------------------------------------------------------
# QuetzComponent — 2 vCPUs
# ---------------------------------------------------------------------------
cpu = sst.Component("cpu", "quetz.QuetzComponent")
cpu.addParams({
    "verbose"          : 1,
    "clock"            : cpu_clock,
    "vcpu_count"       : NUM_VCPUS,
    "maxcorequeue"     : 64,
    "maxtranscore"     : 16,
    "maxissuepercycle" : 2,
    "cachelinesize"    : 64,
    "qemu"             : qemu_bin,
    "qemu_plugin"      : plugin,
    "executable"       : exe,

    # Tell the OpenMP runtime to use 2 threads
    "envparamcount"    : 1,
    "envparamname0"    : "OMP_NUM_THREADS",
    "envparamval0"     : str(NUM_VCPUS),
})
cpu.enableAllStatistics()

# ---------------------------------------------------------------------------
# Per-vCPU L1 caches (each vCPU gets its own private L1)
# ---------------------------------------------------------------------------
l1caches = []
for i in range(NUM_VCPUS):
    l1 = sst.Component(f"l1cache_{i}", "memHierarchy.Cache")
    l1.addParams({
        "access_latency_cycles" : 2,
        "cache_frequency"       : cpu_clock,
        "replacement_policy"    : "lru",
        "coherence_protocol"    : "MSI",
        "associativity"         : 4,
        "cache_line_size"       : 64,
        "cache_size"            : "32KB",
        "L1"                    : 1,
    })
    l1caches.append(l1)

# ---------------------------------------------------------------------------
# Shared bus — all L1 caches connect here
# ---------------------------------------------------------------------------
bus = sst.Component("bus", "memHierarchy.Bus")
bus.addParams({"bus_frequency": cpu_clock})

# ---------------------------------------------------------------------------
# MemController (single, shared by all vCPUs via bus)
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
for i in range(NUM_VCPUS):
    sst.Link(f"cpu_to_l1_{i}").connect(
        (cpu,         f"cache_link_{i}", "1ns"),
        (l1caches[i], "highlink",        "1ns"))

    sst.Link(f"l1_{i}_to_bus").connect(
        (l1caches[i], "lowlink",         "5ns"),
        (bus,         f"highlink{i}",    "5ns"))

sst.Link("bus_to_mem").connect(
    (bus,     "lowlink0", "1ns"),
    (memctrl, "highlink", "1ns"))

sst.setProgramOption("timebase", "1ps")
sst.setStatisticLoadLevel(4)
sst.setStatisticOutput("sst.statOutputConsole")
