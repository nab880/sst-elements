"""
preset_quetz_sysmode.py — minimal sysmode SDL exercising QuetzConfigManager
platform presets.

Sets only the bare minimum params (qemu binary path, plugin path, executable,
and platform name).  QuetzConfigManager pulls system_mode, system_mode_loader,
qemu_args, isa, and region_handler presets from the named platform, demonstrating
equivalent behaviour to explicit region_handler wiring in basic_quetz_sysmode.py.

Required environment:
  QUETZ_EXE       Path to the guest firmware binary.
  QUETZ_QEMU      Path to qemu-system-* binary.
  QUETZ_PLATFORM  Name of a registered platform preset.

Optional:
  QUETZ_PLUGIN    libqemu_sst_plugin.so (default: $SST_HOME/libexec).
  QUETZ_RAM_START / QUETZ_RAM_END  MemController address range
                  (default 0 - 0xFFFFFFFF).
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

def _parse_addr(s):
    s = s.strip()
    return int(s, 16) if s.startswith("0x") or s.startswith("0X") else int(s)

sst_home   = _sst_home()
exe        = os.environ.get("QUETZ_EXE",       "")
qemu_bin   = os.environ.get("QUETZ_QEMU",      "")
plugin     = os.environ.get("QUETZ_PLUGIN",
                             os.path.join(sst_home, "libexec", "libqemu_sst_plugin.so"))
platform   = os.environ.get("QUETZ_PLATFORM",  "")
ram_start  = _parse_addr(os.environ.get("QUETZ_RAM_START", "0"))
ram_end    = _parse_addr(os.environ.get("QUETZ_RAM_END",   "0xFFFFFFFF"))

if not exe:
    raise RuntimeError("QUETZ_EXE is not set")
if not qemu_bin:
    raise RuntimeError("QUETZ_QEMU is not set")
if not platform:
    raise RuntimeError("QUETZ_PLATFORM is not set")
if not os.path.exists(exe):
    raise FileNotFoundError(f"QUETZ_EXE not found: {exe}")
if not os.path.exists(qemu_bin):
    raise FileNotFoundError(f"QUETZ_QEMU not found: {qemu_bin}")

cpu = sst.Component("cpu", "quetz.QuetzComponent")
cpu.addParams({
    "verbose"          : 1,
    "platform"         : platform,
    "qemu"             : qemu_bin,
    "qemu_plugin"      : plugin,
    "executable"       : exe,
    "maxissuepercycle" : 2,
})
cpu.enableAllStatistics()

memctrl = sst.Component("memory", "memHierarchy.MemController")
memctrl.addParams({
    "clock"            : "1GHz",
    "addr_range_start" : ram_start,
    "addr_range_end"   : ram_end,
})
mem_be = memctrl.setSubComponent("backend", "memHierarchy.simpleMem")
mem_be.addParams({
    "access_time" : "100ns",
    "mem_size"    : str(ram_end - ram_start + 1) + "B",
})

sst.Link("cpu_to_mem").connect(
    (cpu,     "cache_link_0", "1ns"),
    (memctrl, "highlink",     "1ns"))

sst.setProgramOption("timebase", "1ps")
sst.setStatisticLoadLevel(4)
sst.setStatisticOutput("sst.statOutputConsole")
