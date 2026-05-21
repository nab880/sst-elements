"""
test_config_bad.py — expect fatal error when compute_latency_* is set
without detailed_instruction_tracking.
"""

import sst
import os
import shutil
import subprocess

def _sst_prefix():
    if "SST_HOME" in os.environ:
        return os.environ["SST_HOME"]
    cfg = shutil.which("sst-config")
    if cfg:
        return subprocess.check_output([cfg, "--prefix"], text=True).strip()
    raise RuntimeError("Set SST_HOME or put sst-config on PATH")

sst_home = _sst_prefix()
plugin   = os.path.join(sst_home, "libexec", "libqemu_sst_plugin.so")
qemu_bin = os.environ.get("QUETZ_QEMU", os.path.join(sst_home, "bin", "qemu-riscv64"))
exe      = os.environ.get("QUETZ_EXE", "")

if not exe:
    raise RuntimeError("QUETZ_EXE must be set by the testsuite")

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
    "compute_latency_int" : 10,
    "detailed_instruction_tracking" : 0,
})

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
