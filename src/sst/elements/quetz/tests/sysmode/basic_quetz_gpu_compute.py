"""
basic_quetz_gpu_compute.py — QuetzGpuDevice in COMPUTE mode (quetz.FFTKernel
in the 'kernel' slot).

Unlike basic_quetz_gpu.py (the device is a pure latency model and the guest CPU
computes), here the DEVICE actually computes the FFT: on the doorbell it DMA-reads
the input buffer, computes in C++, and DMA-writes the result. The guest CPU only
fills the input, programs 3 registers, rings the doorbell, and reads the output —
no FFT math on the guest.

Memory map (Option A — SST-backed buffer window, rest filtered):
  0x10000000  UART                         (uart handler)
  0x00100000  SiFive test finisher         (filtered)
  0x80000000-0x800FFFFF  guest code/stack  (FILTERED — lives in QEMU, fast)
  0x80100000-0x801003FF  GPU MMIO registers (MmioForward -> device iface)
  0x90000000-0x9000FFFF  FFT buffer window  (SST-backed DRAM; NOT filtered)
Only the FFT buffers cross the SST memory hierarchy — both the CPU (fill/read) and
the device mem_iface (DMA) reach the window via a small NoC + directory +
MemController. This mirrors balar's isSSTmem H2D/D2H buffers.

The window is trapped in QEMU by a second sst-mmio-bridge aperture (the launcher
creates it from QUETZ_SST_WIN_START/END, which MUST be exported alongside the
MMIO vars) and delivered synchronously through the CPU's mmio interface onto the
NoC; the plugin drops those accesses from the trace ring so they are not
double-issued. QEMU has no RAM at the window address — without the env vars the
guest's first store there faults.

Env: QUETZ_EXE, QUETZ_QEMU, QUETZ_QEMU_ARGS, QUETZ_LOADER, QUETZ_MMIO_START/END,
QUETZ_SST_WIN_START/END, QUETZ_FFT_LATENCY_COEFF.
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
qemu_args  = os.environ.get("QUETZ_QEMU_ARGS", "-machine virt -nographic -bios none")
loader     = os.environ.get("QUETZ_LOADER", "-kernel")
clock      = os.environ.get("QUETZ_CLOCK", "1GHz")
ram_start  = _parse_addr(os.environ.get("QUETZ_RAM_START", "0"))
ram_end    = _parse_addr(os.environ.get("QUETZ_RAM_END", "0xFFFFFFFF"))
mmio_start = _parse_addr(os.environ.get("QUETZ_MMIO_START", "0x80100000"))
mmio_end   = _parse_addr(os.environ.get("QUETZ_MMIO_END", "0x801003FF"))
win_start  = _parse_addr(os.environ.get("QUETZ_SST_WIN_START", "0x90000000"))
win_end    = _parse_addr(os.environ.get("QUETZ_SST_WIN_END", "0x9000FFFF"))
win_size   = win_end - win_start + 1

if not exe:
    raise RuntimeError("QUETZ_EXE is not set")
if not qemu_bin:
    raise RuntimeError("QUETZ_QEMU is not set")

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
})

# Region handlers (first match wins). The FFT window needs no handler here: its
# accesses never reach the trace path (the plugin drops them — they arrive via
# the sync-MMIO mailbox instead and are forwarded over the CPU's mmio interface
# to the directory/MemController, so both CPU and device see the same bytes).
mmio_rh = cpu.setSubComponent("region_handler", "quetz.MmioForwardRegionHandler", 0)
mmio_rh.addParams({"start": mmio_start, "end": mmio_end})
fin_rh = cpu.setSubComponent("region_handler", "quetz.FilteredRegionHandler", 1)
fin_rh.addParams({"start": 0x100000, "end": 0x100003})
uart_rh = cpu.setSubComponent("region_handler", "quetz.UartRegionHandler", 2)
uart_rh.addParams({"start": 0x10000000, "end": 0x10000FFF})
kernel_rh = cpu.setSubComponent("region_handler", "quetz.FilteredRegionHandler", 3)
kernel_rh.addParams({"start": 0x80000000, "end": 0x800FFFFF})
subram_rh = cpu.setSubComponent("region_handler", "quetz.FilteredRegionHandler", 4)
subram_rh.addParams({"start": ram_start, "end": 0x7FFFFFFF})
cpu.enableAllStatistics()

# CPU cache path onto the NoC (reaches the FFT window MemController).
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

# CPU MMIO on the NoC (balar pattern): the doorbell/register writes reach the GPU
# MMIO target through the router, and the GPU's mem_iface reaches DRAM the same way.
cpu_mmio_if = cpu.setSubComponent("mmio", "memHierarchy.standardInterface", 0)
cpu_mmio_nic = cpu_mmio_if.setSubComponent("lowlink", "memHierarchy.MemNIC")
cpu_mmio_nic.addParams({"group": CORE_GROUP, "destinations": CORE_DST, "network_bw": NETWORK_BW})

# GPU device: MMIO target (iface) + memory initiator (mem_iface) for kernel DMA.
gpu = sst.Component("gpu", "quetz.QuetzGpuDevice")
gpu.addParams({
    "base_addr": mmio_start,
    "mmio_size": (mmio_end - mmio_start + 1),
    "clock": "1GHz",
    "doorbell_blocking": 1,   # required when a kernel is loaded
    # Kernel DMA may only touch the SST-backed window: a guest-programmed
    # buffer address outside it rejects the op (gpu.ops_rejected) instead
    # of crashing the simulation in memHierarchy routing.
    "dma_range_start": win_start,
    "dma_range_end": win_end,
})
gpu.enableAllStatistics()
# Kernel selection: any quetz.QuetzKernel subclass (QUETZ_KERNEL env).
kernel_name = os.environ.get("QUETZ_KERNEL", "quetz.FFTKernel")
gpu_kernel = gpu.setSubComponent("kernel", kernel_name)
if kernel_name == "quetz.FFTKernel":
    gpu_kernel.addParams({
        "fft_latency_coeff": int(os.environ.get("QUETZ_FFT_LATENCY_COEFF", "20")),
    })
gpu_mmio_if = gpu.setSubComponent("iface", "memHierarchy.standardInterface")
gpu_mmio_nic = gpu_mmio_if.setSubComponent("lowlink", "memHierarchy.MemNIC")
gpu_mmio_nic.addParams({"group": MMIO_GROUP, "sources": MMIO_SRC,
                        "destinations": MMIO_DST, "network_bw": NETWORK_BW})
gpu_mem_if  = gpu.setSubComponent("mem_iface", "memHierarchy.standardInterface")
gpu_mem_nic = gpu_mem_if.setSubComponent("lowlink", "memHierarchy.MemNIC")
gpu_mem_nic.addParams({"group": CORE_GROUP, "destinations": CORE_DST, "network_bw": NETWORK_BW})

chiprtr = sst.Component("quetz_gpu_chiprtr", "merlin.hr_router")
chiprtr.addParams({
    "xbar_bw": NOC_BW,
    "id": "0",
    "input_buf_size": "1KB",
    "num_ports": "5",
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

# All initiators + the directory share the NoC (balar pattern). The GPU MMIO
# window and the FFT DRAM window are both reached through the router.
sst.Link("cpu_l1").connect((cpu, "cache_link_0", "1ns"), (l1_cpu, "port", "1ns"))
sst.Link("l1_rtr").connect((l1_nic, "port", "1ns"), (chiprtr, "port0", "1ns"))
sst.Link("cpu_mmio_rtr").connect((cpu_mmio_nic, "port", "1ns"), (chiprtr, "port1", "1ns"))
sst.Link("gpu_mmio_rtr").connect((gpu_mmio_nic, "port", "1ns"), (chiprtr, "port2", "1ns"))
sst.Link("gpu_mem_rtr").connect((gpu_mem_nic, "port", "1ns"), (chiprtr, "port3", "1ns"))
sst.Link("dir_rtr").connect((dir_nic, "port", "1ns"), (chiprtr, "port4", "1ns"))
sst.Link("mem_bus").connect((mem_hi, "port", "1ns"), (directory, "lowlink", "1ns"))

sst.setProgramOption("timebase", "1ps")
sst.setStatisticLoadLevel(4)
sst.setStatisticOutput("sst.statOutputConsole")
