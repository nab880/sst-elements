"""
basic_quetz_sysmode.py — parameterized SST SDL for Quetz system-mode tests.

Configured via environment variables set by testsuite_default_quetz.py.

Region handlers (optional):
  QUETZ_REGION_HANDLER_COUNT
  QUETZ_REGION_HANDLER{n}_TYPE   quetz.FilteredRegionHandler | UartRegionHandler | ...
  QUETZ_REGION_HANDLER{n}_START  address (int or 0x hex)
  QUETZ_REGION_HANDLER{n}_END
  QUETZ_REGION_HANDLER{n}_TX_OFFSET  (uart only, default 0)
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

_LEGACY_TYPE = {
    "filtered": "quetz.FilteredRegionHandler",
    "uart":     "quetz.UartRegionHandler",
    "memory":   "quetz.ForwardRegionHandler",
}

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
platform   = os.environ.get("QUETZ_PLATFORM",   "")

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
}
if platform:
    cpu_params["platform"] = platform
if stdin_file:
    cpu_params["appstdin"] = stdin_file
if stdout_file:
    cpu_params["appstdout"] = stdout_file

cpu = sst.Component("cpu", "quetz.QuetzComponent")
cpu.addParams(cpu_params)

rh_count = int(os.environ.get("QUETZ_REGION_HANDLER_COUNT", "0"))
for n in range(rh_count):
    pfx = f"QUETZ_REGION_HANDLER{n}_"
    rh_type = os.environ.get(pfx + "TYPE", "")
    if rh_type in _LEGACY_TYPE:
        rh_type = _LEGACY_TYPE[rh_type]
    if not rh_type:
        continue
    rh = cpu.setSubComponent("region_handler", rh_type, n)
    rh.addParams({
        "start": _parse_addr(os.environ.get(pfx + "START", "0")),
        "end":   _parse_addr(os.environ.get(pfx + "END",   "0")),
    })
    tx = os.environ.get(pfx + "TX_OFFSET", "")
    if tx:
        rh.addParams({"tx_offset": int(tx)})

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
