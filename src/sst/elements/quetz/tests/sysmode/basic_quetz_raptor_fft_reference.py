"""Dedicated Raptor/Quetz deck for the FFT polling reference.

The accelerator and DMA window are simulator-owned execution apertures.  They
are deliberately outside the BSP-derived Raptor map and are not silicon claims.
The guest is a normal app_core1 BSP ELF using the stock CRT and UART1 driver.
"""

import os
import sys

import sst

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from quetz_test_helpers import sst_prefix  # noqa: E402


P1_RAM_START = 0x80000000
P1_RAM_END = 0x8000FFFF
UART1_BASE = 0xFC064000

FFT_MMIO_START = 0x70000000
FFT_MMIO_END = 0x700000FF
FFT_WINDOW_START = 0x71000000
FFT_WINDOW_END = 0x71000FFF
FFT_INPUT_ADDRESS = 0x71000000
FFT_OUTPUT_ADDRESS = 0x71000800
FFT_BUFFER_BYTES = 2048

QEMU_ARGS = (
    "-machine raptor-core2,strict-mmio=on "
    "-icount shift=10,sleep=off "
    "-display none -monitor none "
    "-serial null -serial stdio -serial null"
)

CORE_GROUP = 0
MMIO_GROUP = 1
MEMORY_GROUP = 2
CORE_DST = [MEMORY_GROUP, MMIO_GROUP]
MMIO_SRC = [CORE_GROUP]
MMIO_DST = [MEMORY_GROUP]
MEMORY_SRC = [CORE_GROUP, MMIO_GROUP]
NETWORK_BW = "25GB/s"
NOC_BW = "128GB/s"


def _parse_addr(value):
    value = str(value).strip()
    return int(value, 16) if value.startswith(("0x", "0X")) else int(value)


def _fixed_addr(name, expected):
    actual = _parse_addr(os.environ.get(name, hex(expected)))
    if actual != expected:
        raise RuntimeError(
            f"Raptor FFT reference requires {name}=0x{expected:08x}, got "
            f"0x{actual:08x}"
        )
    os.environ[name] = f"0x{expected:08x}"
    return actual


sst_home = sst_prefix()
executable = os.environ.get("QUETZ_EXE", "")
qemu = os.environ.get("QUETZ_QEMU", "qemu-system-m68k")
plugin = os.environ.get(
    "QUETZ_PLUGIN", os.path.join(sst_home, "libexec", "libqemu_sst_plugin.so")
)
loader = os.environ.get("QUETZ_LOADER", "-kernel")
clock = os.environ.get("QUETZ_CLOCK", "1GHz")
stdout_file = os.environ.get("QUETZ_STDOUT_FILE", "")
event_file = os.environ.get("QUETZ_ACCELERATOR_EVENTS_FILE", "")

ram_start = _fixed_addr("QUETZ_RAM_START", P1_RAM_START)
ram_end = _fixed_addr("QUETZ_RAM_END", P1_RAM_END)
uart_base = _fixed_addr("QUETZ_UART_ADDR", UART1_BASE)
mmio_start = _fixed_addr("QUETZ_MMIO_START", FFT_MMIO_START)
mmio_end = _fixed_addr("QUETZ_MMIO_END", FFT_MMIO_END)
win_start = _fixed_addr("QUETZ_SST_WIN_START", FFT_WINDOW_START)
win_end = _fixed_addr("QUETZ_SST_WIN_END", FFT_WINDOW_END)
win_size = win_end - win_start + 1

if not executable:
    raise RuntimeError("QUETZ_EXE is not set")
if not os.path.isfile(executable):
    raise FileNotFoundError(f"QUETZ_EXE not found: {executable}")
if not event_file:
    raise RuntimeError(
        "Raptor FFT reference requires QUETZ_ACCELERATOR_EVENTS_FILE"
    )
if os.environ.get("QUETZ_BSP_PROFILE") or os.environ.get("QUETZ_BSP_DISCOVER") == "1":
    raise RuntimeError("Raptor FFT reference rejects BSP compatibility overlays")
if os.environ.get("QUETZ_MMIO_PAYLOAD", "1") != "1":
    raise RuntimeError("Raptor FFT reference requires QUETZ_MMIO_PAYLOAD=1")
os.environ["QUETZ_MMIO_PAYLOAD"] = "1"

if FFT_INPUT_ADDRESS % 64 or FFT_OUTPUT_ADDRESS % 64:
    raise RuntimeError("FFT buffers must be 64-byte aligned")
if FFT_INPUT_ADDRESS + FFT_BUFFER_BYTES != FFT_OUTPUT_ADDRESS:
    raise RuntimeError("FFT reference buffers must be adjacent and non-overlapping")
if FFT_OUTPUT_ADDRESS + FFT_BUFFER_BYTES != FFT_WINDOW_END + 1:
    raise RuntimeError("FFT reference buffers must exactly fill the DMA window")

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
    "window_big_endian": 1,
}
if stdout_file:
    cpu_params["appstdout"] = stdout_file

cpu = sst.Component("cpu", "quetz.QuetzComponent")
cpu.addParams(cpu_params)

# First match wins. The two bridge apertures are served synchronously and are
# dropped from the trace ring, but declaring MMIO here keeps the trace contract
# explicit if that transport changes.
mmio = cpu.setSubComponent("region_handler", "quetz.MmioForwardRegionHandler", 0)
mmio.addParams({"start": FFT_MMIO_START, "end": FFT_MMIO_END})
uart = cpu.setSubComponent("region_handler", "quetz.UartRegionHandler", 1)
uart.addParams({"start": UART1_BASE, "end": UART1_BASE + 0x1F, "tx_offset": 0x0C})
soc = cpu.setSubComponent("region_handler", "quetz.FilteredRegionHandler", 2)
soc.addParams({"start": 0xFC000000, "end": 0xFCFFFFFF})
ram = cpu.setSubComponent("region_handler", "quetz.ForwardRegionHandler", 3)
ram.addParams({"start": P1_RAM_START, "end": P1_RAM_END})
cpu.addParams({"filter_unmatched_regions": 1})
cpu.enableAllStatistics()

# P1 is QEMU-owned; this path records timing only. The FFT data uses the
# separate coherent SST window below.
p1_memory = sst.Component("p1_memory", "memHierarchy.MemController")
p1_memory.addParams(
    {
        "clock": "1GHz",
        "addr_range_start": P1_RAM_START,
        "addr_range_end": P1_RAM_END,
    }
)
p1_backend = p1_memory.setSubComponent("backend", "memHierarchy.simpleMem")
p1_backend.addParams({"access_time": "100ns", "mem_size": "65536B"})
sst.Link("cpu_to_p1_memory").connect(
    (cpu, "cache_link_0", "1ns"),
    (p1_memory, "highlink", "1ns"),
)

# Synchronous guest MMIO/window accesses share this NoC with accelerator DMA.
cpu_mmio_if = cpu.setSubComponent("mmio", "memHierarchy.standardInterface", 0)
cpu_mmio_nic = cpu_mmio_if.setSubComponent("lowlink", "memHierarchy.MemNIC")
cpu_mmio_nic.addParams(
    {"group": CORE_GROUP, "destinations": CORE_DST, "network_bw": NETWORK_BW}
)

gpu = sst.Component("gpu", "quetz.QuetzGpuDevice")
gpu.addParams(
    {
        "base_addr": FFT_MMIO_START,
        "mmio_size": FFT_MMIO_END - FFT_MMIO_START + 1,
        "clock": "1GHz",
        "kernel_latency": 5000,
        "doorbell_blocking": 0,
        "dma_range_start": FFT_WINDOW_START,
        "dma_range_end": FFT_WINDOW_END,
        "data_big_endian": 1,
        "event_file": event_file,
        "event_source": "accelerator.fft",
        "event_operation": "fft",
    }
)
gpu.enableAllStatistics()
gpu_kernel = gpu.setSubComponent("kernel", "quetz.FFTKernel")
# Long enough to make BUSY observable; this is functional latency, not timing.
gpu_kernel.addParams({"fft_latency_coeff": 100})
gpu_mmio_if = gpu.setSubComponent("iface", "memHierarchy.standardInterface")
gpu_mmio_nic = gpu_mmio_if.setSubComponent("lowlink", "memHierarchy.MemNIC")
gpu_mmio_nic.addParams(
    {
        "group": MMIO_GROUP,
        "sources": MMIO_SRC,
        "destinations": MMIO_DST,
        "network_bw": NETWORK_BW,
    }
)
gpu_mem_if = gpu.setSubComponent("mem_iface", "memHierarchy.standardInterface")
gpu_mem_nic = gpu_mem_if.setSubComponent("lowlink", "memHierarchy.MemNIC")
gpu_mem_nic.addParams(
    {"group": CORE_GROUP, "destinations": CORE_DST, "network_bw": NETWORK_BW}
)

router = sst.Component("fft_router", "merlin.hr_router")
router.addParams(
    {
        "xbar_bw": NOC_BW,
        "id": "0",
        "input_buf_size": "1KB",
        "num_ports": "4",
        "flit_size": "72B",
        "output_buf_size": "1KB",
        "link_bw": NOC_BW,
        "topology": "merlin.singlerouter",
    }
)
router.setSubComponent("topology", "merlin.singlerouter")

fft_memory = sst.Component("fft_memory", "memHierarchy.MemController")
fft_memory.addParams(
    {
        "clock": "1GHz",
        "addr_range_start": FFT_WINDOW_START,
        "addr_range_end": FFT_WINDOW_END,
    }
)
fft_backend = fft_memory.setSubComponent("backend", "memHierarchy.simpleMem")
fft_backend.addParams({"access_time": "100ns", "mem_size": str(win_size) + "B"})
fft_memory_link = fft_memory.setSubComponent("highlink", "memHierarchy.MemLink")

directory = sst.Component("fft_directory", "memHierarchy.DirectoryController")
directory.addParams(
    {
        "clock": "1GHz",
        "coherence_protocol": "MESI",
        "cache_line_size": 64,
        "entry_cache_size": 4096,
        "mshr_num_entries": 256,
        "addr_range_start": FFT_WINDOW_START,
        "addr_range_end": FFT_WINDOW_END,
    }
)
directory_nic = directory.setSubComponent("highlink", "memHierarchy.MemNIC")
directory_nic.addParams(
    {"group": MEMORY_GROUP, "sources": MEMORY_SRC, "network_bw": NETWORK_BW}
)

sst.Link("cpu_mmio_router").connect(
    (cpu_mmio_nic, "port", "1ns"), (router, "port0", "1ns")
)
sst.Link("gpu_mmio_router").connect(
    (gpu_mmio_nic, "port", "1ns"), (router, "port1", "1ns")
)
sst.Link("gpu_dma_router").connect(
    (gpu_mem_nic, "port", "1ns"), (router, "port2", "1ns")
)
sst.Link("directory_router").connect(
    (directory_nic, "port", "1ns"), (router, "port3", "1ns")
)
sst.Link("fft_memory_bus").connect(
    (fft_memory_link, "port", "1ns"), (directory, "lowlink", "1ns")
)

sst.setProgramOption("timebase", "1ps")
sst.setStatisticLoadLevel(4)
stats_out = os.environ.get("QUETZ_STATS_OUT", "")
if stats_out:
    sst.setStatisticOutput("sst.statOutputCSV", {"filepath": stats_out})
else:
    sst.setStatisticOutput("sst.statOutputConsole")
