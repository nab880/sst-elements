"""
basic_quetz_gpu_compute_coldfire.py — QuetzGpuDevice in COMPUTE mode for a
ColdFire (m68k) guest.

The m68k counterpart of basic_quetz_gpu_compute.py: the DEVICE computes the FFT
(quetz.FFTKernel in the 'kernel' slot — DMA-read input, kernel compute,
DMA-write result); the guest only fills the input, programs 3 registers, rings
the doorbell, and verifies.

ColdFire mcf5208evb map:
  0xfc060000  on-chip UART                  (uart handler, tx at +0x0C)
  0x80000000  TestFinisher sentinel         (testfinisher handler)
  0x40000000-0x47FFFFFF  SDRAM code/stack   (FILTERED — lives in QEMU, fast)
  0x70000000-0x700003FF  GPU MMIO registers (MmioForward -> device iface)
  0x71000000-0x7100FFFF  FFT buffer window  (SST-backed DRAM; NOT filtered)

The window is trapped in QEMU by a second sst-mmio-bridge aperture (the launcher
creates it from QUETZ_SST_WIN_START/END, which MUST be exported alongside the
MMIO vars) and delivered synchronously through the CPU's mmio interface onto the
NoC; the plugin drops those accesses from the trace ring.

Window byte layout defaults to big-endian in this ColdFire deck: the CPU uses
`window_big_endian=1` and the GPU uses matching `data_big_endian=1`, so mixed-
size aliases match real m68k memory. Set QUETZ_WIN_BIG_ENDIAN=0 only to test the
generic legacy little-endian layout. Numeric same-size round-trips work in
either mode.

Env: QUETZ_EXE, QUETZ_QEMU(=qemu-system-m68k), QUETZ_QEMU_ARGS, QUETZ_LOADER,
QUETZ_MMIO_START/END, QUETZ_SST_WIN_START/END, QUETZ_FFT_LATENCY_COEFF,
QUETZ_KERNEL_BIG_ENDIAN (independent override of the device's data_big_endian,
default: matches QUETZ_WIN_BIG_ENDIAN — set it differently only to reproduce
the documented mismatched-flags footgun).

Optional stimulus/response devices for the full-loop demo (coldfire_accel_sink):
QUETZ_SENSOR_FILE adds a quetz.QuetzStreamDevice at QUETZ_SENSOR_BASE
(default 0x70010000) and QUETZ_SINK_FILE a quetz.QuetzSinkDevice at
QUETZ_SINK_BASE (default 0x70020000), both as additional MMIO targets on the
NoC — set QUETZ_MMIO_END=0x7002FFFF so the bridge window reaches them (the
GPU register block then stays 0x400 bytes instead of owning the window).
"""

import os
import sst

import sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from quetz_test_helpers import sst_prefix  # noqa: E402


def _parse_addr(s):
    s = str(s).strip()
    return int(s, 16) if s.startswith(("0x", "0X")) else int(s)


CORE_GROUP = 0
MMIO_GROUP = 1
MEMORY_GROUP = 2
CORE_DST = [MEMORY_GROUP, MMIO_GROUP]
MMIO_SRC = [CORE_GROUP]
MMIO_DST = [MEMORY_GROUP]
MEMORY_SRC = [CORE_GROUP, MMIO_GROUP]
NETWORK_BW = "25GB/s"
NOC_BW = "128GB/s"

sst_home   = sst_prefix()
exe        = os.environ.get("QUETZ_EXE", "")
qemu_bin   = os.environ.get("QUETZ_QEMU", "")
plugin     = os.environ.get("QUETZ_PLUGIN",
                            os.path.join(sst_home, "libexec", "libqemu_sst_plugin.so"))
qemu_args  = os.environ.get("QUETZ_QEMU_ARGS",
                            "-machine mcf5208evb -display none -serial stdio -m 128M")
loader     = os.environ.get("QUETZ_LOADER", "-kernel")
clock      = os.environ.get("QUETZ_CLOCK", "1GHz")
ram_start  = _parse_addr(os.environ.get("QUETZ_RAM_START", "0x40000000"))
ram_end    = _parse_addr(os.environ.get("QUETZ_RAM_END", "0x47FFFFFF"))
mmio_start = _parse_addr(os.environ.get("QUETZ_MMIO_START", "0x70000000"))
mmio_end   = _parse_addr(os.environ.get("QUETZ_MMIO_END", "0x700003FF"))
win_start  = _parse_addr(os.environ.get("QUETZ_SST_WIN_START", "0x71000000"))
win_end    = _parse_addr(os.environ.get("QUETZ_SST_WIN_END", "0x7100FFFF"))
win_size   = win_end - win_start + 1
# BE window byte layout (see the docstring). The CPU-side and kernel-side
# flags MUST agree or the kernel reads byte-swapped values; QUETZ_KERNEL_BIG_ENDIAN
# is an independent override (default: matches win_be) for deliberately
# testing that mismatched-flags failure mode.
win_be     = os.environ.get("QUETZ_WIN_BIG_ENDIAN", "1") == "1"
kernel_be  = os.environ.get("QUETZ_KERNEL_BIG_ENDIAN",
                            "1" if win_be else "0") == "1"
uart_addr  = _parse_addr(os.environ.get("QUETZ_UART_ADDR", "0xfc060000"))
sentinel_addr = _parse_addr(os.environ.get("QUETZ_SENTINEL_ADDR", "0x80000000"))
sensor_file = os.environ.get("QUETZ_SENSOR_FILE", "")
sensor_base = _parse_addr(os.environ.get("QUETZ_SENSOR_BASE", "0x70010000"))
sink_file   = os.environ.get("QUETZ_SINK_FILE", "")
sink_base   = _parse_addr(os.environ.get("QUETZ_SINK_BASE", "0x70020000"))

if not exe:
    raise RuntimeError("QUETZ_EXE is not set")
if not qemu_bin:
    raise RuntimeError("QUETZ_QEMU is not set")
# The launcher only instantiates the sync-MMIO bridge for the GPU register
# range when QUETZ_MMIO_PAYLOAD=1. Without it the doorbell/ARG writes take
# the imprecise trace path and the kernel DMA reads garbage addresses — fail
# here with the actual cause instead of a MemNIC routing fatal mid-run.
if os.environ.get("QUETZ_MMIO_PAYLOAD", "") != "1":
    raise RuntimeError(
        "QUETZ_MMIO_PAYLOAD=1 is required by this deck: the GPU registers "
        "(QUETZ_MMIO_START/END) must be served synchronously by the "
        "sst-mmio-bridge for the kernel doorbell/DMA path to work. Export "
        "QUETZ_MMIO_PAYLOAD=1 (tests: enable_mmio_payload_delivery()).")

cpu = sst.Component("cpu", "quetz.QuetzComponent")
cpu.addParams({
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
    "window_big_endian": 1 if win_be else 0,
})

# Region handlers (first match wins). The FFT window needs no handler: its
# accesses never reach the trace path (the plugin drops them — they arrive via
# the sync-MMIO mailbox and are forwarded over the CPU's mmio interface to the
# directory/MemController). Everything unmatched is filtered (QEMU-serviced).
mmio_rh = cpu.setSubComponent("region_handler", "quetz.MmioForwardRegionHandler", 0)
mmio_rh.addParams({"start": mmio_start, "end": mmio_end})
uart_rh = cpu.setSubComponent("region_handler", "quetz.UartRegionHandler", 1)
uart_rh.addParams({"start": uart_addr, "end": uart_addr + 0xFF, "tx_offset": 0x0C})
fin_rh = cpu.setSubComponent("region_handler", "quetz.TestFinisherRegionHandler", 2)
fin_rh.addParams({"start": sentinel_addr, "end": sentinel_addr + 3})
cpu.addParams({"filter_unmatched_regions": 1})
cpu.enableAllStatistics()

# CPU cache path onto the NoC (unused for traffic here — all guest RAM is
# filtered and window accesses bypass the trace path — but cache_link_0 must be
# connected, and this mirrors basic_quetz_gpu_compute.py).
l1 = sst.Component("l1", "memHierarchy.Cache")
l1.addParams({
    "cache_frequency": clock,
    "cache_size": "32KB",
    "associativity": 4,
    "access_latency_cycles": 2,
    "L1": 1,
    "coherence_protocol": "mesi",
    "replacement_policy": "lru",
    "cache_line_size": 64,
    "addr_range_start": win_start,
    "addr_range_end": win_end,
})
l1_cpu = l1.setSubComponent("highlink", "memHierarchy.MemLink")
l1_nic = l1.setSubComponent("lowlink", "memHierarchy.MemNIC")
l1_nic.addParams({"group": CORE_GROUP, "destinations": CORE_DST, "network_bw": NETWORK_BW})

# CPU MMIO on the NoC (balar pattern): register writes reach the GPU MMIO
# target through the router; window accesses reach the directory the same way.
cpu_mmio_if = cpu.setSubComponent("mmio", "memHierarchy.standardInterface", 0)
cpu_mmio_nic = cpu_mmio_if.setSubComponent("lowlink", "memHierarchy.MemNIC")
cpu_mmio_nic.addParams({"group": CORE_GROUP, "destinations": CORE_DST, "network_bw": NETWORK_BW})

# GPU device: MMIO target (iface) + memory initiator (mem_iface) for kernel DMA.
# Alone it owns the whole bridge window; with stream/sink devices sharing the
# window it keeps just its 0x400-byte register block.
gpu = sst.Component("gpu", "quetz.QuetzGpuDevice")
gpu.addParams({
    "base_addr": mmio_start,
    "mmio_size": 0x400 if (sensor_file or sink_file)
                 else (mmio_end - mmio_start + 1),
    "clock": os.environ.get("QUETZ_GPU_CLOCK", "1GHz"),
    "kernel_latency": int(os.environ.get("QUETZ_GPU_LATENCY", "5000")),
    # Blocking preserves the original demo. Set 0 to acknowledge the submit
    # immediately and exercise guest STATUS polling through DMA writeback.
    "doorbell_blocking": int(os.environ.get("QUETZ_DOORBELL_BLOCKING", "1")),
    # Kernel DMA may only touch the SST-backed window: a guest-programmed
    # buffer address outside it rejects the op (gpu.ops_rejected) instead
    # of crashing the simulation in memHierarchy routing.
    "dma_range_start": win_start,
    "dma_range_end": win_end,
    # Buffer byte layout, pushed by the device into whatever kernel loads
    # below (kernels take no endianness param of their own).
    "data_big_endian": 1 if kernel_be else 0,
})
gpu.enableAllStatistics()
# Kernel selection: any quetz.QuetzKernel subclass (QUETZ_KERNEL env).
kernel_name = os.environ.get("QUETZ_KERNEL", "quetz.FFTKernel")
gpu_kernel = gpu.setSubComponent("kernel", kernel_name)
if kernel_name == "quetz.FFTKernel":
    gpu_kernel.addParams({
        "fft_latency_coeff": int(os.environ.get("QUETZ_FFT_LATENCY_COEFF", "20")),
    })
elif kernel_name == "quetz.ScaleOffsetKernel":
    gpu_kernel.addParams({
        "latency_coeff": int(os.environ.get("QUETZ_SCALE_LATENCY_COEFF", "4")),
    })
gpu_mmio_if = gpu.setSubComponent("iface", "memHierarchy.standardInterface")
gpu_mmio_nic = gpu_mmio_if.setSubComponent("lowlink", "memHierarchy.MemNIC")
gpu_mmio_nic.addParams({"group": MMIO_GROUP, "sources": MMIO_SRC,
                        "destinations": MMIO_DST, "network_bw": NETWORK_BW})
gpu_mem_if  = gpu.setSubComponent("mem_iface", "memHierarchy.standardInterface")
gpu_mem_nic = gpu_mem_if.setSubComponent("lowlink", "memHierarchy.MemNIC")
gpu_mem_nic.addParams({"group": CORE_GROUP, "destinations": CORE_DST, "network_bw": NETWORK_BW})

# Optional stimulus/response devices: extra MMIO targets on the NoC, routed
# by their address regions like the GPU register block.
sensors = None
if sensor_file:
    sensors = sst.Component("sensors", "quetz.QuetzStreamDevice")
    sensors.addParams({
        "base_addr": sensor_base,
        "mmio_size": 0x100,
        "stream_file": sensor_file,
    })
    sensors.enableAllStatistics()
    sensors_if = sensors.setSubComponent("iface", "memHierarchy.standardInterface")
    sensors_nic = sensors_if.setSubComponent("lowlink", "memHierarchy.MemNIC")
    sensors_nic.addParams({"group": MMIO_GROUP, "sources": MMIO_SRC,
                           "destinations": MMIO_DST, "network_bw": NETWORK_BW})

sink = None
if sink_file:
    sink = sst.Component("sink", "quetz.QuetzSinkDevice")
    sink.addParams({
        "base_addr": sink_base,
        "mmio_size": 0x100,
        "sink_file": sink_file,
    })
    sink.enableAllStatistics()
    sink_if = sink.setSubComponent("iface", "memHierarchy.standardInterface")
    sink_nic = sink_if.setSubComponent("lowlink", "memHierarchy.MemNIC")
    sink_nic.addParams({"group": MMIO_GROUP, "sources": MMIO_SRC,
                        "destinations": MMIO_DST, "network_bw": NETWORK_BW})

num_ports = 5 + (1 if sensors is not None else 0) + (1 if sink is not None else 0)
chiprtr = sst.Component("quetz_gpu_chiprtr", "merlin.hr_router")
chiprtr.addParams({
    "xbar_bw": NOC_BW,
    "id": "0",
    "input_buf_size": "1KB",
    "num_ports": str(num_ports),
    "flit_size": "72B",
    "output_buf_size": "1KB",
    "link_bw": NOC_BW,
    "topology": "merlin.singlerouter",
})
chiprtr.setSubComponent("topology", "merlin.singlerouter")

# SST-backed DRAM for the FFT window.
memctrl = sst.Component("memory", "memHierarchy.MemController")
memctrl.addParams({
    "clock": "1GHz",
    "addr_range_start": win_start,
    "addr_range_end": win_end,
})
mem_be = memctrl.setSubComponent("backend", "memHierarchy.simpleMem")
mem_be.addParams({"access_time": "100 ns", "mem_size": str(win_size) + "B"})
mem_hi = memctrl.setSubComponent("highlink", "memHierarchy.MemLink")

directory = sst.Component("directory", "memHierarchy.DirectoryController")
directory.addParams({
    "clock": "1GHz",
    "coherence_protocol": "MESI",
    "cache_line_size": 64,
    "entry_cache_size": 32768,
    "mshr_num_entries": 1024,
    "addr_range_start": win_start,
    "addr_range_end": win_end,
})
dir_nic = directory.setSubComponent("highlink", "memHierarchy.MemNIC")
dir_nic.addParams({"group": MEMORY_GROUP, "sources": MEMORY_SRC, "network_bw": NETWORK_BW})

sst.Link("cpu_l1").connect((cpu, "cache_link_0", "1ns"), (l1_cpu, "port", "1ns"))
sst.Link("l1_rtr").connect((l1_nic, "port", "1ns"), (chiprtr, "port0", "1ns"))
sst.Link("cpu_mmio_rtr").connect((cpu_mmio_nic, "port", "1ns"), (chiprtr, "port1", "1ns"))
sst.Link("gpu_mmio_rtr").connect((gpu_mmio_nic, "port", "1ns"), (chiprtr, "port2", "1ns"))
sst.Link("gpu_mem_rtr").connect((gpu_mem_nic, "port", "1ns"), (chiprtr, "port3", "1ns"))
sst.Link("dir_rtr").connect((dir_nic, "port", "1ns"), (chiprtr, "port4", "1ns"))
_next_port = 5
if sensors is not None:
    sst.Link("sensors_rtr").connect(
        (sensors_nic, "port", "1ns"), (chiprtr, "port%d" % _next_port, "1ns"))
    _next_port += 1
if sink is not None:
    sst.Link("sink_rtr").connect(
        (sink_nic, "port", "1ns"), (chiprtr, "port%d" % _next_port, "1ns"))
    _next_port += 1
sst.Link("mem_bus").connect((mem_hi, "port", "1ns"), (directory, "lowlink", "1ns"))

sst.setProgramOption("timebase", "1ps")
sst.setStatisticLoadLevel(4)
sst.setStatisticOutput("sst.statOutputConsole")
