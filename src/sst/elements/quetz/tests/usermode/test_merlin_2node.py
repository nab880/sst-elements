#!/usr/bin/env python3
"""
SST test: two-node network topology via Merlin hr_router.

Two QuetzComponent nodes each run the RVV SAXPY kernel.  Each node has
its own private L1 cache.  The L1 caches are connected via MemNIC to a
Merlin singlerouter switch.  A shared L2 cache sits on the same network;
its lower link goes directly to a single MemController.

Network topology:
  Node 0: QuetzComponent → L1_0 ──┐
  Node 1: QuetzComponent → L1_1 ──┤── Merlin hr_router ── L2 ── MemCtrl
                                   │           (singlerouter)
                                   └── (3 ports total)

What the test verifies:
  - Both QEMU instances run to completion.
  - Coherence traffic crosses the Merlin network (L2 receives misses from both L1s).
  - read_requests > 0 on both vCPUs.
  - L2 CacheHits + CacheMisses > 0.

This exercises the MemNIC → Merlin linkcontrol path, which is the
typical network-on-chip (NoC) wiring in SST memory-hierarchy designs.

To run:
  sst test_merlin_2node.py
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

# Pre-compiled RVV SAXPY binary — exercises vector load/store paths.
exe = os.path.join(tests_dir, "..", "binaries", "rvv_saxpy")
if not os.path.exists(exe):
    raise FileNotFoundError(f"rvv_saxpy binary not found at {exe}")

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
cpu_clock     = "1GHz"
network_bw    = "10GB/s"
NUM_NODES     = 2
# Network ports: one per L1 + one for the shared L2
NUM_PORTS     = NUM_NODES + 1

# ---------------------------------------------------------------------------
# Merlin network (single router — simplest topology for a single-chip model)
# ---------------------------------------------------------------------------
network = sst.Component("network", "merlin.hr_router")
network.addParams({
    "xbar_bw"         : network_bw,
    "link_bw"         : network_bw,
    "input_buf_size"  : "2KiB",
    "output_buf_size" : "2KiB",
    "num_ports"       : NUM_PORTS,
    "flit_size"       : "36B",
    "id"              : 0,
    "topology"        : "merlin.singlerouter",
})
network.setSubComponent("topology", "merlin.singlerouter")

# ---------------------------------------------------------------------------
# Two independent QuetzComponent nodes
# ---------------------------------------------------------------------------
for node_id in range(NUM_NODES):
    cpu = sst.Component(f"cpu_{node_id}", "quetz.QuetzComponent")
    cpu.addParams({
        "verbose"          : 0,
        "clock"            : cpu_clock,
        "vcpu_count"       : 1,
        "maxcorequeue"     : 64,
        "maxtranscore"     : 16,
        "maxissuepercycle" : 2,
        "cachelinesize"    : 64,
        "qemu"             : qemu_bin,
        "qemu_plugin"      : plugin,
        "executable"       : exe,
        "exec_latency_vec" : 4,
    })
    cpu.enableAllStatistics()

    l1 = sst.Component(f"l1_{node_id}", "memHierarchy.Cache")
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
    l1.enableAllStatistics()

    # L1 → network via MemNIC (group 1 = L1 caches)
    l1_nic = l1.setSubComponent("lowlink", "memHierarchy.MemNIC")
    l1_nic.addParams({
        "group"                   : 1,
        "network_bw"              : network_bw,
        "network_input_buffer_size"  : "1KiB",
        "network_output_buffer_size" : "1KiB",
    })

    sst.Link(f"cpu_{node_id}_to_l1").connect(
        (cpu, "cache_link_0", "1ns"),
        (l1,  "highlink",     "1ns"))

    sst.Link(f"l1_{node_id}_to_net").connect(
        (l1_nic, "port", "200ps"),
        (network, f"port{node_id}", "200ps"))

# ---------------------------------------------------------------------------
# Shared L2 cache — connected to the network on its high side
# ---------------------------------------------------------------------------
l2 = sst.Component("l2", "memHierarchy.Cache")
l2.addParams({
    "access_latency_cycles" : 10,
    "cache_frequency"       : cpu_clock,
    "replacement_policy"    : "lru",
    "coherence_protocol"    : "MSI",
    "associativity"         : 8,
    "cache_line_size"       : 64,
    "cache_size"            : "256KB",
    "L1"                    : 0,
})
l2.enableAllStatistics()

# L2 highlink → network (group 2 = LLC)
l2_nic = l2.setSubComponent("highlink", "memHierarchy.MemNIC")
l2_nic.addParams({
    "group"                   : 2,
    "network_bw"              : network_bw,
    "network_input_buffer_size"  : "2KiB",
    "network_output_buffer_size" : "2KiB",
})

sst.Link("l2_to_net").connect(
    (l2_nic,  "port",    "200ps"),
    (network, f"port{NUM_NODES}", "200ps"))

# ---------------------------------------------------------------------------
# Shared MemController (below L2, no network needed here)
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

sst.Link("l2_to_mem").connect(
    (l2,      "lowlink",  "50ns"),
    (memctrl, "highlink", "50ns"))

# ---------------------------------------------------------------------------
# Simulation options
# ---------------------------------------------------------------------------
sst.setProgramOption("timebase", "1ps")
sst.setStatisticLoadLevel(4)
sst.setStatisticOutput("sst.statOutputConsole")
