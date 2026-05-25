"""
basic_quetz_gpu.py — sysmode SDL for QuetzGpuDevice (P2.a kernel-latency test).

Slot 0 is reserved for the MmioForwardRegionHandler on the GPU MMIO window.
QUETZ_REGION_HANDLER{n}_* entries (n >= 0) are loaded into slots 1+.

Expects QUETZ_* env vars like basic_quetz_sysmode.py, plus:
  QUETZ_MMIO_START / QUETZ_MMIO_END  — MMIO window for handler + GPU device
  QUETZ_GPU_LATENCY                  — default kernel_latency cycles (optional)
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
qemu_args  = os.environ.get("QUETZ_QEMU_ARGS", "-machine virt -nographic -bios none")
loader     = os.environ.get("QUETZ_LOADER",    "-kernel")
clock      = os.environ.get("QUETZ_CLOCK",     "1GHz")
ram_start  = _parse_addr(os.environ.get("QUETZ_RAM_START", "0"))
ram_end    = _parse_addr(os.environ.get("QUETZ_RAM_END",   "0xFFFFFFFF"))
mmio_start = _parse_addr(os.environ.get("QUETZ_MMIO_START", "0x80100000"))
mmio_end   = _parse_addr(os.environ.get("QUETZ_MMIO_END",   "0x801003FF"))
platform   = os.environ.get("QUETZ_PLATFORM",   "")

if not exe:
    raise RuntimeError("QUETZ_EXE is not set")
if not qemu_bin:
    raise RuntimeError("QUETZ_QEMU is not set")

cpu_params = {
    "verbose"           : 1,
    "clock"             : clock,
    "vcpu_count"        : 1,
    "maxcorequeue"      : 64,
    "maxtranscore"      : 16,
    "maxissuepercycle" : 2,
    "cachelinesize"     : 64,
    "qemu"              : qemu_bin,
    "qemu_plugin"       : plugin,
    "executable"        : exe,
    "system_mode"       : 1,
    "system_mode_loader": loader,
    "qemu_args"         : qemu_args,
}
if platform:
    cpu_params["platform"] = platform

cpu = sst.Component("cpu", "quetz.QuetzComponent")
cpu.addParams(cpu_params)

mmio_rh = cpu.setSubComponent("region_handler", "quetz.MmioForwardRegionHandler", 0)
mmio_rh.addParams({"start": mmio_start, "end": mmio_end})

rh_count = int(os.environ.get("QUETZ_REGION_HANDLER_COUNT", "0"))
for n in range(rh_count):
    pfx = f"QUETZ_REGION_HANDLER{n}_"
    rh_type = os.environ.get(pfx + "TYPE", "")
    _LEGACY = {
        "filtered": "quetz.FilteredRegionHandler",
        "uart":     "quetz.UartRegionHandler",
        "memory":   "quetz.ForwardRegionHandler",
        "mmio":     "quetz.MmioForwardRegionHandler",
    }
    if rh_type in _LEGACY:
        rh_type = _LEGACY[rh_type]
    if not rh_type:
        continue
    slot = n + 1
    rh = cpu.setSubComponent("region_handler", rh_type, slot)
    rh.addParams({
        "start": _parse_addr(os.environ.get(pfx + "START", "0")),
        "end":   _parse_addr(os.environ.get(pfx + "END",   "0")),
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

gpu = sst.Component("gpu", "quetz.QuetzGpuDevice")
gpu.addParams({
    "base_addr": mmio_start,
    "mmio_size": (mmio_end - mmio_start + 1),
    "kernel_latency": int(os.environ.get("QUETZ_GPU_LATENCY", "5000")),
    "clock": "1GHz",
})
gpu.enableAllStatistics()
gpu_if = gpu.setSubComponent("iface", "memHierarchy.standardInterface")

sst.Link("cpu_to_mem").connect(
    (cpu,     "cache_link_0", "1ns"),
    (memctrl, "highlink",     "1ns"))

sst.Link("cpu_to_gpu").connect(
    (cpu,    "mmio_link_0", "1ns"),
    (gpu_if, "port",        "1ns"))

sst.setProgramOption("timebase", "1ps")
sst.setStatisticLoadLevel(4)
sst.setStatisticOutput("sst.statOutputConsole")
