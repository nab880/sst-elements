"""Dedicated SST deck for the Raptor Level-2 BSP self-test.

The Raptor QEMU machine owns every guest-visible peripheral.  SST observes
UART1 and times P1 RAM accesses, but this deck deliberately creates no
TestFinisher, compatibility overlay, MMIO bridge, accelerator, or stimulus
device.  The host runner stops the simulation when the reviewed UART
completion line is observed and then evaluates the checked-in oracle.
"""

import os
import sys

import sst

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from quetz_test_helpers import sst_prefix  # noqa: E402


P1_RAM_START = 0x80000000
P1_RAM_END = 0x8000FFFF
UART1_BASE = 0xFC064000
QEMU_ARGS = (
    "-machine raptor-core2,strict-mmio=on "
    "-icount shift=10,sleep=off "
    "-display none -monitor none "
    "-serial null -serial stdio -serial null"
)


def _parse_addr(value):
    value = str(value).strip()
    return int(value, 16) if value.startswith(("0x", "0X")) else int(value)


here = os.path.dirname(os.path.abspath(__file__))
sst_home = sst_prefix()
executable = os.environ.get("QUETZ_EXE", "")
qemu = os.environ.get("QUETZ_QEMU", "qemu-system-m68k")
plugin = os.environ.get(
    "QUETZ_PLUGIN", os.path.join(sst_home, "libexec", "libqemu_sst_plugin.so")
)
loader = os.environ.get("QUETZ_LOADER", "-kernel")
clock = os.environ.get("QUETZ_CLOCK", "1GHz")
stdout_file = os.environ.get("QUETZ_STDOUT_FILE", "")
ram_start = _parse_addr(os.environ.get("QUETZ_RAM_START", hex(P1_RAM_START)))
ram_end = _parse_addr(os.environ.get("QUETZ_RAM_END", hex(P1_RAM_END)))
uart_base = _parse_addr(os.environ.get("QUETZ_UART_ADDR", hex(UART1_BASE)))

if not executable:
    raise RuntimeError("QUETZ_EXE is not set")
if not os.path.isfile(executable):
    raise FileNotFoundError(f"QUETZ_EXE not found: {executable}")
if ram_start != P1_RAM_START or ram_end != P1_RAM_END:
    raise RuntimeError(
        "Raptor Level-2 requires P1 RAM 0x80000000..0x8000ffff, got "
        f"0x{ram_start:08x}..0x{ram_end:08x}"
    )
if uart_base != UART1_BASE:
    raise RuntimeError(
        f"Raptor Level-2 requires UART1 at 0xfc064000, got 0x{uart_base:08x}"
    )
if os.environ.get("QUETZ_BSP_PROFILE") or os.environ.get("QUETZ_BSP_DISCOVER") == "1":
    raise RuntimeError("Raptor Level-2 rejects BSP compatibility overlays")
if os.environ.get("QUETZ_SST_WIN_START") or os.environ.get("QUETZ_SST_WIN_END"):
    raise RuntimeError("Raptor Level-2 rejects SST-backed guest apertures")

# quetz-run enables payload forwarding for legacy decks by default.  Disable
# it here before Quetz launches QEMU: the native machine has no synthetic MMIO
# bridge and the self-test needs no SST-side peripheral.
os.environ["QUETZ_MMIO_PAYLOAD"] = "0"

cpu_params = {
    "verbose": 1,
    "clock": clock,
    "vcpu_count": 1,
    "maxcorequeue": 64,
    "maxtranscore": 16,
    "maxissuepercycle": 2,
    "cachelinesize": 64,
    "qemu": qemu,
    "qemu_plugin": plugin,
    "executable": executable,
    "system_mode": 1,
    "system_mode_loader": loader,
    "qemu_args": QEMU_ARGS,
}
if stdout_file:
    cpu_params["appstdout"] = stdout_file

cpu = sst.Component("cpu", "quetz.QuetzComponent")
cpu.addParams(cpu_params)

# QEMU services the UART.  The trace handler captures UART1 TX bytes and keeps
# its MMIO traffic out of the RAM timing path.
uart = cpu.setSubComponent("region_handler", "quetz.UartRegionHandler", 0)
uart.addParams({"start": UART1_BASE, "end": UART1_BASE + 0x1F, "tx_offset": 0x0C})

# Other native Raptor peripheral accesses are observed but never forwarded to
# a synthetic SST device.  Unsupported accesses still fail inside QEMU because
# strict-mmio is pinned in QEMU_ARGS.
soc = cpu.setSubComponent("region_handler", "quetz.FilteredRegionHandler", 1)
soc.addParams({"start": 0xFC000000, "end": 0xFCFFFFFF})

ram = cpu.setSubComponent("region_handler", "quetz.ForwardRegionHandler", 2)
ram.addParams({"start": P1_RAM_START, "end": P1_RAM_END})
cpu.addParams({"filter_unmatched_regions": 1})
cpu.enableAllStatistics()

memory = sst.Component("memory", "memHierarchy.MemController")
memory.addParams(
    {
        "clock": "1GHz",
        "addr_range_start": P1_RAM_START,
        "addr_range_end": P1_RAM_END,
    }
)
backend = memory.setSubComponent("backend", "memHierarchy.simpleMem")
backend.addParams({"access_time": "100ns", "mem_size": "65536B"})

sst.Link("cpu_to_memory").connect(
    (cpu, "cache_link_0", "1ns"),
    (memory, "highlink", "1ns"),
)

sst.setProgramOption("timebase", "1ps")
sst.setStatisticLoadLevel(4)
stats_out = os.environ.get("QUETZ_STATS_OUT", "")
if stats_out:
    sst.setStatisticOutput("sst.statOutputCSV", {"filepath": stats_out})
else:
    sst.setStatisticOutput("sst.statOutputConsole")
