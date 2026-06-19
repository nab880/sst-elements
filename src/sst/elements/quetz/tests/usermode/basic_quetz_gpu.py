"""
basic_quetz_gpu.py — user-mode SDL for QuetzGpuDevice (mmio_link kernel-latency test).

Slot 0 is reserved for the MmioForwardRegionHandler on the GPU MMIO window.
QUETZ_REGION_HANDLER{n}_* entries (n >= 0) are loaded into slots 1+.

Expects QUETZ_* env vars like basic_quetz.py, plus:
  QUETZ_MMIO_START / QUETZ_MMIO_END  — MMIO window for handler + GPU device
  QUETZ_GPU_LATENCY                  — default kernel_latency cycles (optional)
  QUETZ_REGION_HANDLER_COUNT         — extra region handlers in slots 1+
  QUETZ_REGION_HANDLER{n}_TYPE/START/END
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
    "mmio":     "quetz.MmioForwardRegionHandler",
    "gpu_trace": "quetz.GpuTraceRegionHandler",
}

sst_home   = _sst_home()
exe        = os.environ.get("QUETZ_EXE",       "")
qemu_bin   = os.environ.get("QUETZ_QEMU",      "")
plugin     = os.environ.get("QUETZ_PLUGIN",
                             os.path.join(sst_home, "libexec", "libqemu_sst_plugin.so"))
clock      = os.environ.get("QUETZ_CLOCK",     "1GHz")
ram_start  = _parse_addr(os.environ.get("QUETZ_RAM_START", "0"))
ram_end    = _parse_addr(os.environ.get("QUETZ_RAM_END",   str((1 << 48) - 1)))
mmio_start = _parse_addr(os.environ.get("QUETZ_MMIO_START", "0x80100000"))
mmio_end   = _parse_addr(os.environ.get("QUETZ_MMIO_END",   "0x801003FF"))

if not exe:
    raise RuntimeError("QUETZ_EXE is not set")
if not qemu_bin:
    raise RuntimeError("QUETZ_QEMU is not set")
if not os.path.exists(exe):
    raise FileNotFoundError(f"QUETZ_EXE not found: {exe}")
if not os.path.exists(qemu_bin):
    raise FileNotFoundError(f"QUETZ_QEMU not found: {qemu_bin}")

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
}
isa = os.environ.get("QUETZ_ISA", "")
if isa:
    cpu_params["isa"] = isa
if os.environ.get("QUETZ_DETAILED", "0") == "1":
    cpu_params["detailed_instruction_tracking"] = 1

cpu = sst.Component("cpu", "quetz.QuetzComponent")
cpu.addParams(cpu_params)

mmio_rh = cpu.setSubComponent("region_handler", "quetz.MmioForwardRegionHandler", 0)
mmio_rh.addParams({"start": mmio_start, "end": mmio_end})

rh_count = int(os.environ.get("QUETZ_REGION_HANDLER_COUNT", "0"))
for n in range(rh_count):
    pfx = f"QUETZ_REGION_HANDLER{n}_"
    rh_type = os.environ.get(pfx + "TYPE", "")
    if rh_type in _LEGACY_TYPE:
        rh_type = _LEGACY_TYPE[rh_type]
    if not rh_type:
        continue
    slot = n + 1
    rh = cpu.setSubComponent("region_handler", rh_type, slot)
    rh.addParams({
        "start": _parse_addr(os.environ.get(pfx + "START", "0")),
        "end":   _parse_addr(os.environ.get(pfx + "END",   "0")),
    })
    _rh_extra = {
        "TX_OFFSET":       "tx_offset",
        "DOORBELL_OFFSET": "doorbell_offset",
        "STATUS_OFFSET":   "status_offset",
        "MAX_PAYLOAD_LOG": "max_payload_log",
    }
    for env_key, param_name in _rh_extra.items():
        val = os.environ.get(pfx + env_key, "")
        if val:
            rh.addParams({param_name: int(val)})

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
