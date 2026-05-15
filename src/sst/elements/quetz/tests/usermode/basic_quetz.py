"""
basic_quetz.py — parameterized SST SDL for Quetz user-mode tests.

Configured entirely via environment variables set by the test harness
(testsuite_default_quetz.py).  Can also be run directly with sst if the
variables are set manually.

Required:
  QUETZ_EXE        Path to the guest binary.
  QUETZ_QEMU       Path to the QEMU user-mode binary.

Optional:
  QUETZ_PLUGIN     Path to libqemu_sst_plugin.so (default: $SST_HOME/libexec)
  QUETZ_WITH_L1    Set to "1" to insert a 32 KiB MSI L1 cache. Default: "0"
  QUETZ_VCPU_COUNT Number of vCPUs. Default: "1"
  QUETZ_ISA        ISA string forwarded to QuetzComponent. Default: ""
  QUETZ_CLOCK      CPU clock rate. Default: "1GHz"
  SST_HOME         SST installation prefix (used to locate the plugin).
"""

import sst
import os

def _sst_home():
    h = os.environ.get("SST_HOME", "")
    if h:
        return h
    import shutil, subprocess
    cfg = shutil.which("sst-config")
    if cfg:
        return subprocess.check_output([cfg, "--prefix"], text=True).strip()
    raise RuntimeError("Set SST_HOME or put sst-config on PATH")

sst_home    = _sst_home()
exe         = os.environ.get("QUETZ_EXE",    "")
qemu_bin    = os.environ.get("QUETZ_QEMU",   "")
plugin      = os.environ.get("QUETZ_PLUGIN", os.path.join(sst_home, "libexec", "libqemu_sst_plugin.so"))
with_l1     = os.environ.get("QUETZ_WITH_L1", "0") == "1"
vcpu_count  = int(os.environ.get("QUETZ_VCPU_COUNT", "1"))
isa         = os.environ.get("QUETZ_ISA",    "")
clock       = os.environ.get("QUETZ_CLOCK",  "1GHz")

if not exe:
    raise RuntimeError("QUETZ_EXE is not set")
if not qemu_bin:
    raise RuntimeError("QUETZ_QEMU is not set")
if not os.path.exists(exe):
    raise FileNotFoundError(f"QUETZ_EXE not found: {exe}")
if not os.path.exists(qemu_bin):
    raise FileNotFoundError(f"QUETZ_QEMU not found: {qemu_bin}")

cpu_params = {
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
}
if isa:
    cpu_params["isa"] = isa

cpu = sst.Component("cpu", "quetz.QuetzComponent")
cpu.addParams(cpu_params)
cpu.enableAllStatistics()

memctrl = sst.Component("memory", "memHierarchy.MemController")
memctrl.addParams({
    "clock"            : "1GHz",
    "addr_range_start" : 0,
    "addr_range_end"   : (1 << 48) - 1,
})
mem_be = memctrl.setSubComponent("backend", "memHierarchy.simpleMem")
mem_be.addParams({"access_time": "100ns", "mem_size": "256TiB"})

if with_l1:
    l1 = sst.Component("l1cache", "memHierarchy.Cache")
    l1.addParams({
        "access_latency_cycles" : 2,
        "cache_frequency"       : "1GHz",
        "replacement_policy"    : "lru",
        "coherence_protocol"    : "MSI",
        "associativity"         : 4,
        "cache_line_size"       : 64,
        "cache_size"            : "32KB",
        "L1"                    : 1,
    })
    sst.Link("cpu_l1").connect((cpu, "cache_link_0", "1ns"), (l1, "highlink", "1ns"))
    sst.Link("l1_mem").connect((l1,  "lowlink",  "50ns"), (memctrl, "highlink", "50ns"))
else:
    sst.Link("cpu_mem").connect((cpu, "cache_link_0", "1ns"), (memctrl, "highlink", "1ns"))

sst.setProgramOption("timebase", "1ps")
sst.setStatisticLoadLevel(4)
sst.setStatisticOutput("sst.statOutputConsole")
