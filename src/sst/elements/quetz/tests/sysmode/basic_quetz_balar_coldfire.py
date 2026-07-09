"""
basic_quetz_balar_coldfire.py — sysmode SDL wiring a ColdFire (m68k) Quetz guest
to balar/GPGPU-Sim.

Same Quetz->balar fabric as basic_quetz_balar.py, but for the NXP ColdFire
mcf5208evb memory map: firmware + packet scratch live in SDRAM at 0x40000000
(below the balar MMIO window at 0x70000000), so a single coherent DRAM range is
used instead of the RISC-V low/high split. The on-chip UART is at 0xfc060000 and
the test-finisher sentinel is the on-chip SRAM at 0x80000000.

Env: QUETZ_EXE, QUETZ_QEMU(=qemu-system-m68k), QUETZ_QEMU_ARGS(=-M mcf5208evb
-nographic -m 128M), QUETZ_LOADER(=-kernel), QUETZ_STDIN_FILE (UART RX script),
BALAR_CUDA_EXE_PATH (the vectorAdd fatbin), BALAR_CONFIG (gpu cfg).
"""

import os
import sys

import sst


def _sst_home():
    h = os.environ.get("SST_HOME", "")
    if h:
        return h
    import shutil
    import subprocess
    cfg = shutil.which("sst-config")
    if cfg:
        return subprocess.check_output([cfg, "--prefix"], text=True).strip()
    raise RuntimeError("Set SST_HOME or put sst-config on PATH")


def _parse_addr(s):
    s = str(s).strip()
    return int(s, 16) if s.startswith("0x") or s.startswith("0X") else int(s)


THIS_DIR = os.path.dirname(os.path.abspath(__file__))
BALAR_TESTS_DIR = os.path.abspath(
    os.path.join(THIS_DIR, "..", "..", "..", "balar", "tests"))
if not os.path.isfile(os.path.join(BALAR_TESTS_DIR, "balarBlock.py")):
    raise RuntimeError("balarBlock.py not found at {}".format(BALAR_TESTS_DIR))
sys.path.insert(0, BALAR_TESTS_DIR)
import balarBlock  # noqa: E402


CORE_GROUP = 0
MMIO_GROUP = 1
MEMORY_GROUP = 2
CORE_DST = [MEMORY_GROUP, MMIO_GROUP]
MMIO_SRC = [CORE_GROUP]
MMIO_DST = [MEMORY_GROUP]
MEMORY_SRC = [CORE_GROUP, MMIO_GROUP]
NETWORK_BW = "25GB/s"
CLOCK = os.environ.get("QUETZ_CLOCK", "1GHz")

# P1 host<->GPU interconnect model. balar's doorbell (host MMIO write) and the GPU
# DMA engine's host-memory accesses (H2D/D2H) cross a modeled PCIe/NVLink-style
# link that is distinct from the on-chip NoC. Defaults approximate PCIe gen3 x16.
GPU_LINK_LATENCY = os.environ.get("QUETZ_GPU_LINK_LATENCY", "500ns")
GPU_LINK_BW = os.environ.get("QUETZ_GPU_LINK_BW", "16GB/s")
# The NoC must out-run the GPU link so the modeled PCIe bandwidth is the binding
# constraint on the GPU path (the prior 1GB/s router throttled everything alike).
NOC_BW = os.environ.get("QUETZ_NOC_BW", "128GB/s")

sst_home = _sst_home()
exe = os.environ.get("QUETZ_EXE", "")
qemu_bin = os.environ.get("QUETZ_QEMU", "")
plugin = os.environ.get(
    "QUETZ_PLUGIN", os.path.join(sst_home, "libexec", "libqemu_sst_plugin.so"))
# Use `-serial stdio` (not `-nographic`): -nographic muxes the QEMU monitor onto
# the serial (mon:stdio), which starves UART RX delivery so the interactive
# monitor never receives typed commands. `-display none -serial stdio` wires
# UART0 straight to stdio and RX works reliably.
qemu_args = os.environ.get(
    "QUETZ_QEMU_ARGS", "-machine mcf5208evb -display none -serial stdio -m 128M")
loader = os.environ.get("QUETZ_LOADER", "-kernel")
stdin_file = os.environ.get("QUETZ_STDIN_FILE", "")
stdout_file = os.environ.get("QUETZ_STDOUT_FILE", "")

if not exe:
    raise RuntimeError("QUETZ_EXE is not set")
if not qemu_bin:
    raise RuntimeError("QUETZ_QEMU is not set")

# ColdFire mcf5208evb map: SDRAM at 0x40000000, on-chip UART0 at 0xfc060000,
# SRAM (test-finisher sentinel) at 0x80000000. balar MMIO is a disjoint peer.
balar_mmio_addr = _parse_addr(os.environ.get("BALAR_MMIO_ADDR", "0x70000000"))
balar_mmio_size = _parse_addr(os.environ.get("BALAR_MMIO_SIZE", "0x400"))
dma_mmio_addr = _parse_addr(
    os.environ.get("BALAR_DMA_MMIO_ADDR", hex(balar_mmio_addr + balar_mmio_size)))
dma_mmio_size = _parse_addr(os.environ.get("BALAR_DMA_MMIO_SIZE", "0x200"))
mmio_start = _parse_addr(os.environ.get("QUETZ_MMIO_START", hex(balar_mmio_addr)))
mmio_end = _parse_addr(
    os.environ.get("QUETZ_MMIO_END", hex(dma_mmio_addr + dma_mmio_size - 1)))

ram_start = _parse_addr(os.environ.get("QUETZ_RAM_START", "0x40000000"))
ram_end = _parse_addr(os.environ.get("QUETZ_RAM_END", "0x47FFFFFF"))
uart_addr = _parse_addr(os.environ.get("QUETZ_UART_ADDR", "0xfc060000"))
sentinel_addr = _parse_addr(os.environ.get("QUETZ_SENTINEL_ADDR", "0x80000000"))

cfg_file = os.environ.get(
    "BALAR_CONFIG", os.path.join(BALAR_TESTS_DIR, "gpu-v100-mem.cfg"))
cuda_exe = os.environ.get(
    "BALAR_CUDA_EXE_PATH",
    os.path.join(BALAR_TESTS_DIR, "balar_trace", "vectorAdd"))

cpu_params = {
    "verbose": 1,
    "clock": CLOCK,
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
    "balar_doorbell_addr": balar_mmio_addr,
    "balar_doorbell_size": 8,
    "balar_packet_flush_bytes": 4096,
}
if stdin_file:
    cpu_params["appstdin"] = stdin_file
if stdout_file:
    cpu_params["appstdout"] = stdout_file

# P4 async offload (see basic_quetz_balar.py): a Quetz-emulated submit aperture
# 0x100 into balar's MMIO region lets the guest post a blocking call and poll for
# completion. The QEMU bridge already covers it; balar never sees it.
if os.environ.get("QUETZ_ASYNC_OFFLOAD", "0") == "1":
    cpu_params["async_offload"] = 1
    cpu_params["async_doorbell_addr"] = balar_mmio_addr + 0x100
    cpu_params["async_doorbell_size"] = 0x40

cpu = sst.Component("cpu", "quetz.QuetzComponent")
cpu.addParams(cpu_params)

# First-match-wins; the doorbell/uart/sentinel must precede the RAM forward.
mmio_rh = cpu.setSubComponent("region_handler", "quetz.MmioForwardRegionHandler", 0)
mmio_rh.addParams({"start": mmio_start, "end": mmio_end})
uart_rh = cpu.setSubComponent("region_handler", "quetz.UartRegionHandler", 1)
uart_rh.addParams({"start": uart_addr, "end": uart_addr + 0xFF, "tx_offset": 0x0C})
fin_rh = cpu.setSubComponent("region_handler", "quetz.TestFinisherRegionHandler", 2)
fin_rh.addParams({"start": sentinel_addr, "end": sentinel_addr + 3})
# RAM forwards to the MemController explicitly (balar packet staging lives
# there); everything else is filtered by the CPU's no-match policy so a wild
# guest access becomes a counted statistic instead of a memHierarchy fatal
# for an unowned address.
ram_rh = cpu.setSubComponent("region_handler", "quetz.ForwardRegionHandler", 3)
ram_rh.addParams({"start": ram_start, "end": ram_end})
cpu.addParams({"filter_unmatched_regions": 1})
cpu.enableAllStatistics()

balar_builder = balarBlock.Builder({"BALAR_CUDA_EXE_PATH": cuda_exe})
balar, balar_mmio_iface = balar_builder.build(
    cfg_file, balar_mmio_addr, dma_mmio_addr,
    verbosity=int(os.environ.get("BALAR_VERBOSE", "0")))
balar.addParams({"compact_return_value": True})

dma = sst.Component("dmaEngine", "balar.dmaEngine")
dma.addParams({
    "verbose": int(os.environ.get("BALAR_DMA_VERBOSE", "0")),
    "clock": balarBlock.clock,
    "mmio_addr": dma_mmio_addr,
    "mmio_size": dma_mmio_size,
})
dma_mmio_if = dma.setSubComponent("mmio_iface", "memHierarchy.standardInterface")
dma_mem_if = dma.setSubComponent("mem_iface", "memHierarchy.standardInterface")

l1 = sst.Component("l1", "memHierarchy.Cache")
l1.addParams({
    "cache_frequency": balarBlock.clock,
    "cache_size": "32KB",
    "associativity": 4,
    "access_latency_cycles": 2,
    "L1": 1,
    "coherence_protocol": "mesi",
    "replacement_policy": "lru",
    "cache_line_size": 64,
    "addr_range_start": ram_start,
    "addr_range_end": ram_end,
})
l1_cpu = l1.setSubComponent("highlink", "memHierarchy.MemLink")
l1_nic = l1.setSubComponent("lowlink", "memHierarchy.MemNIC")
l1_nic.addParams({"group": CORE_GROUP, "destinations": CORE_DST, "network_bw": NETWORK_BW})

cpu_mmio_if = cpu.setSubComponent("mmio", "memHierarchy.standardInterface", 0)
cpu_mmio_nic = cpu_mmio_if.setSubComponent("lowlink", "memHierarchy.MemNIC")
cpu_mmio_nic.addParams({"group": CORE_GROUP, "destinations": CORE_DST, "network_bw": NETWORK_BW})

balar_mmio_nic = balar_mmio_iface.setSubComponent("lowlink", "memHierarchy.MemNIC")
balar_mmio_nic.addParams({"group": MMIO_GROUP, "sources": MMIO_SRC, "destinations": MMIO_DST, "network_bw": GPU_LINK_BW})

dma_mem_nic = dma_mem_if.setSubComponent("lowlink", "memHierarchy.MemNIC")
dma_mem_nic.addParams({"group": CORE_GROUP, "destinations": CORE_DST, "network_bw": GPU_LINK_BW})

dma_mmio_nic = dma_mmio_if.setSubComponent("lowlink", "memHierarchy.MemNIC")
dma_mmio_nic.addParams({"group": MEMORY_GROUP, "sources": MEMORY_SRC, "network_bw": NETWORK_BW})

chiprtr = sst.Component("quetz_balar_chiprtr", "merlin.hr_router")
chiprtr.addParams({
    "xbar_bw": NOC_BW,
    "id": "0",
    "input_buf_size": "1KB",
    "num_ports": "6",
    "flit_size": "72B",
    "output_buf_size": "1KB",
    "link_bw": NOC_BW,
    "topology": "merlin.singlerouter",
})
chiprtr.setSubComponent("topology", "merlin.singlerouter")

memctrl = sst.Component("memory", "memHierarchy.MemController")
memctrl.addParams({
    "clock": "1GHz",
    "addr_range_start": ram_start,
    "addr_range_end": ram_end,
})
mem_be = memctrl.setSubComponent("backend", "memHierarchy.simpleMem")
mem_be.addParams({"access_time": "100 ns", "mem_size": str(ram_end - ram_start + 1) + "B"})
mem_hi = memctrl.setSubComponent("highlink", "memHierarchy.MemLink")

directory = sst.Component("directory", "memHierarchy.DirectoryController")
directory.addParams({
    "clock": "1GHz",
    "coherence_protocol": "MESI",
    "cache_line_size": 64,
    "entry_cache_size": 32768,
    "mshr_num_entries": 1024,  # deep enough to absorb the fast NoC without NACKing balar's DMA
    "addr_range_start": ram_start,
    "addr_range_end": ram_end,
})
dir_nic = directory.setSubComponent("highlink", "memHierarchy.MemNIC")
dir_nic.addParams({"group": MEMORY_GROUP, "sources": MEMORY_SRC, "network_bw": NETWORK_BW})

sst.Link("cpu_l1").connect((cpu, "cache_link_0", "1ns"), (l1_cpu, "port", "1ns"))
sst.Link("mem_bus").connect((mem_hi, "port", "1ns"), (directory, "lowlink", "1ns"))
sst.Link("quetz_l1_rtr").connect((l1_nic, "port", "1ns"), (chiprtr, "port0", "1ns"))
sst.Link("quetz_cpu_mmio_rtr").connect((cpu_mmio_nic, "port", "1ns"), (chiprtr, "port1", "1ns"))
sst.Link("quetz_balar_mmio_rtr").connect((balar_mmio_nic, "port", GPU_LINK_LATENCY), (chiprtr, "port2", "1ns"))
sst.Link("quetz_dma_mem_rtr").connect((dma_mem_nic, "port", GPU_LINK_LATENCY), (chiprtr, "port3", "1ns"))
sst.Link("quetz_dma_mmio_rtr").connect((dma_mmio_nic, "port", "1ns"), (chiprtr, "port4", "1ns"))
sst.Link("quetz_dir_rtr").connect((dir_nic, "port", "1ns"), (chiprtr, "port5", "1ns"))

sst.setProgramOption("timebase", "1ps")
sst.setStatisticLoadLevel(4)
sst.setStatisticOutput("sst.statOutputConsole")
