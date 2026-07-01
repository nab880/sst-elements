"""
basic_quetz_gpu_coldfire.py — synthetic QuetzGpuDevice for a ColdFire (m68k) guest.

The m68k counterpart of basic_quetz_gpu.py: a QuetzGpuDevice (pure latency model)
plus the ColdFire mcf5208evb region handlers. balar-free — nothing here imports
balarBlock (unlike basic_quetz_balar_coldfire.py), so it runs anywhere
qemu-system-m68k exists, with no GPGPU-Sim.

ColdFire mcf5208evb map: SDRAM at 0x40000000 (firmware + FFT arrays), on-chip UART
at 0xfc060000, TestFinisher sentinel (on-chip SRAM) at 0x80000000. The synthetic
GPU MMIO window is a disjoint peer at 0x70000000. Guest RAM is FILTERED (serviced
by QEMU); the FFT compute runs in QEMU memory, only the doorbells/UART are modeled.

Env: QUETZ_EXE, QUETZ_QEMU(=qemu-system-m68k), QUETZ_QEMU_ARGS
(=-machine mcf5208evb -display none -serial stdio -m 128M), QUETZ_LOADER(=-kernel),
QUETZ_GPU_LATENCY, QUETZ_MMIO_START/END, QUETZ_RAM_START/END, QUETZ_UART_ADDR,
QUETZ_SENTINEL_ADDR.
"""

import os
import sst

import sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from quetz_test_helpers import sst_prefix  # noqa: E402


def _parse_addr(s):
    s = str(s).strip()
    return int(s, 16) if s.startswith(("0x", "0X")) else int(s)


sst_home   = sst_prefix()
exe        = os.environ.get("QUETZ_EXE", "")
qemu_bin   = os.environ.get("QUETZ_QEMU", "")
plugin     = os.environ.get("QUETZ_PLUGIN",
                            os.path.join(sst_home, "libexec", "libqemu_sst_plugin.so"))
qemu_args  = os.environ.get("QUETZ_QEMU_ARGS",
                            "-machine mcf5208evb -display none -serial stdio -m 128M")
loader     = os.environ.get("QUETZ_LOADER", "-kernel")
clock      = os.environ.get("QUETZ_CLOCK", "1GHz")
stdin_file = os.environ.get("QUETZ_STDIN_FILE", "")
stdout_file = os.environ.get("QUETZ_STDOUT_FILE", "")

mmio_start = _parse_addr(os.environ.get("QUETZ_MMIO_START", "0x70000000"))
mmio_end   = _parse_addr(os.environ.get("QUETZ_MMIO_END", "0x700003FF"))
ram_start  = _parse_addr(os.environ.get("QUETZ_RAM_START", "0x40000000"))
ram_end    = _parse_addr(os.environ.get("QUETZ_RAM_END", "0x47FFFFFF"))
uart_addr  = _parse_addr(os.environ.get("QUETZ_UART_ADDR", "0xfc060000"))
sentinel_addr = _parse_addr(os.environ.get("QUETZ_SENTINEL_ADDR", "0x80000000"))

if not exe:
    raise RuntimeError("QUETZ_EXE is not set")
if not qemu_bin:
    raise RuntimeError("QUETZ_QEMU is not set")

cpu_params = {
    "verbose": 1,
    "clock": clock,
    "vcpu_count": 1,
    "maxcorequeue": 64,
    "maxtranscore": 16,
    "maxissuepercycle": 2,
    "cachelinesize": 64,
    "qemu": qemu_bin,
    "qemu_plugin": plugin,
    "executable": exe,
    "system_mode": 1,
    "system_mode_loader": loader,
    "qemu_args": qemu_args,
}
if stdin_file:
    cpu_params["appstdin"] = stdin_file
if stdout_file:
    cpu_params["appstdout"] = stdout_file

cpu = sst.Component("cpu", "quetz.QuetzComponent")
cpu.addParams(cpu_params)

# First-match-wins; doorbell/uart/sentinel must precede the DRAM default. Guest
# RAM is not given a handler here — it is serviced by QEMU (filtered) and the
# cache_link path only carries the (unused-for-RAM) default.
mmio_rh = cpu.setSubComponent("region_handler", "quetz.MmioForwardRegionHandler", 0)
mmio_rh.addParams({"start": mmio_start, "end": mmio_end})
uart_rh = cpu.setSubComponent("region_handler", "quetz.UartRegionHandler", 1)
uart_rh.addParams({"start": uart_addr, "end": uart_addr + 0xFF, "tx_offset": 0x0C})
fin_rh = cpu.setSubComponent("region_handler", "quetz.TestFinisherRegionHandler", 2)
fin_rh.addParams({"start": sentinel_addr, "end": sentinel_addr + 3})
cpu.enableAllStatistics()

memctrl = sst.Component("memory", "memHierarchy.MemController")
memctrl.addParams({
    "clock": "1GHz",
    "addr_range_start": ram_start,
    "addr_range_end": ram_end,
})
mem_be = memctrl.setSubComponent("backend", "memHierarchy.simpleMem")
mem_be.addParams({"access_time": "100ns", "mem_size": str(ram_end - ram_start + 1) + "B"})

gpu = sst.Component("gpu", "quetz.QuetzGpuDevice")
gpu.addParams({
    "base_addr": mmio_start,
    "mmio_size": (mmio_end - mmio_start + 1),
    "kernel_latency": int(os.environ.get("QUETZ_GPU_LATENCY", "5000")),
    "clock": "1GHz",
    "doorbell_blocking": 0,
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
