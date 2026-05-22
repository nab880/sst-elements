"""
custom_pipeline_output.py — verify pipeline_output SubComponent slot wiring.

Uses quetz.LoggingPipelineOutput on vCPU 0.  Requires a guest binary via
QUETZ_EXE / QUETZ_QEMU (same as basic_quetz.py).
"""

import sst
import os
import sys

# Reuse env setup from basic_quetz
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from basic_quetz import _sst_home, exe, qemu_bin, plugin, clock, vcpu_count

if not exe or not qemu_bin:
    raise RuntimeError("Set QUETZ_EXE and QUETZ_QEMU (see basic_quetz.py)")

cpu = sst.Component("cpu", "quetz.QuetzComponent")
cpu.addParams({
    "verbose"          : 1,
    "clock"            : clock,
    "vcpu_count"       : vcpu_count,
    "maxcorequeue"     : 64,
    "maxtranscore"     : 16,
    "maxissuepercycle" : 2,
    "cachelinesize"    : 64,
    "qemu"             : qemu_bin,
    "qemu_plugin"      : plugin,
    "executable"       : exe,
})

out_stage = cpu.setSubComponent("pipeline_output", "quetz.LoggingPipelineOutput", 0)

memctrl = sst.Component("memory", "memHierarchy.MemController")
memctrl.addParams({
    "clock"            : "1GHz",
    "addr_range_start" : 0,
    "addr_range_end"   : (1 << 48) - 1,
})
mem_be = memctrl.setSubComponent("backend", "memHierarchy.simpleMem")
mem_be.addParams({"access_time": "100ns", "mem_size": "256TiB"})

sst.Link("cpu_mem").connect((cpu, "cache_link_0", "1ns"), (memctrl, "highlink", "1ns"))

sst.setProgramOption("timebase", "1ps")
sst.setStatisticLoadLevel(4)
sst.setStatisticOutput("sst.statOutputConsole")
