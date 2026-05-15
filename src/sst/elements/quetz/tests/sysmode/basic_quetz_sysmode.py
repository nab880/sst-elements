"""
basic_quetz_sysmode.py — parameterized SST SDL for Quetz system-mode tests.

Configured entirely via environment variables set by the test harness
(testsuite_default_quetz.py).  Can also be run directly with sst if the
variables are set manually.

Required:
  QUETZ_EXE        Path to the guest firmware binary.
  QUETZ_QEMU       Path to the QEMU system-emulation binary (qemu-system-*).

Optional:
  QUETZ_PLUGIN     Path to libqemu_sst_plugin.so (default: $SST_HOME/libexec)
  QUETZ_QEMU_ARGS  Space-separated extra QEMU args (machine/cpu flags).
                   Default: "-machine sifive_u -nographic -bios none"
  QUETZ_LOADER     Flag inserted before the executable: -kernel (default)
                   or -bios (for MIPS Malta raw images).
  QUETZ_CLOCK      CPU clock rate.  Default: "1GHz"
  QUETZ_RAM_START  MemController addr_range_start (int or 0x hex).
                   Default: 0
  QUETZ_RAM_END    MemController addr_range_end (int or 0x hex).
                   Default: 0xFFFFFFFF
  QUETZ_MEMMAP_COUNT  Number of memmap filter regions.  Default: 0
  QUETZ_MEMMAP{n}_NAME   Name of region n.
  QUETZ_MEMMAP{n}_START  Inclusive start address (int or 0x hex).
  QUETZ_MEMMAP{n}_END    Inclusive end address (int or 0x hex).
  QUETZ_MEMMAP{n}_TYPE   Region type: filtered | uart | memory.
  QUETZ_STDIN_FILE  Redirect QEMU stdin from this file (for UART input).
  QUETZ_STDOUT_FILE Redirect QEMU stdout to this file (for UART output capture).
  SST_HOME          SST installation prefix (used to locate the plugin).
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
qemu_args  = os.environ.get("QUETZ_QEMU_ARGS", "-machine sifive_u -nographic -bios none")
loader     = os.environ.get("QUETZ_LOADER",    "-kernel")
clock      = os.environ.get("QUETZ_CLOCK",     "1GHz")
ram_start  = _parse_addr(os.environ.get("QUETZ_RAM_START", "0"))
ram_end    = _parse_addr(os.environ.get("QUETZ_RAM_END",   "0xFFFFFFFF"))
stdin_file = os.environ.get("QUETZ_STDIN_FILE",  "")
stdout_file= os.environ.get("QUETZ_STDOUT_FILE", "")

if not exe:
    raise RuntimeError("QUETZ_EXE is not set")
if not qemu_bin:
    raise RuntimeError("QUETZ_QEMU is not set")
if not os.path.exists(exe):
    raise FileNotFoundError(f"QUETZ_EXE not found: {exe}")
if not os.path.exists(qemu_bin):
    raise FileNotFoundError(f"QUETZ_QEMU not found: {qemu_bin}")

# ---------------------------------------------------------------------------
# Memmap regions
# ---------------------------------------------------------------------------
memmap_count = int(os.environ.get("QUETZ_MEMMAP_COUNT", "0"))
cpu_params = {
    "verbose"          : 1,
    "clock"            : clock,
    "vcpu_count"       : 1,
    "maxcorequeue"     : 64,
    "maxtranscore"     : 16,
    "maxissuepercycle" : 2,
    "cachelinesize"    : 64,
    "qemu"             : qemu_bin,
    "qemu_plugin"      : plugin,
    "executable"       : exe,
    "system_mode"      : 1,
    "system_mode_loader": loader,
    "qemu_args"        : qemu_args,
    "memmap_count"     : memmap_count,
}

for n in range(memmap_count):
    pfx = f"QUETZ_MEMMAP{n}_"
    cpu_params[f"memmap{n}_name"]  = os.environ.get(pfx + "NAME",  f"region{n}")
    cpu_params[f"memmap{n}_start"] = _parse_addr(os.environ.get(pfx + "START", "0"))
    cpu_params[f"memmap{n}_end"]   = _parse_addr(os.environ.get(pfx + "END",   "0"))
    cpu_params[f"memmap{n}_type"]  = os.environ.get(pfx + "TYPE",  "filtered")

if stdin_file:
    cpu_params["appstdin"] = stdin_file
if stdout_file:
    cpu_params["appstdout"] = stdout_file

# ---------------------------------------------------------------------------
# QuetzComponent
# ---------------------------------------------------------------------------
cpu = sst.Component("cpu", "quetz.QuetzComponent")
cpu.addParams(cpu_params)
cpu.enableAllStatistics()

# ---------------------------------------------------------------------------
# MemController (simpleMem backend)
# ---------------------------------------------------------------------------
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
