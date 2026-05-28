"""
basic_quetz_balar.py — sysmode SDL for Quetz -> Balar/GPGPU-Sim wiring.

QEMU sysmode guest writes BalarCudaCallPacket_t structs into guest DRAM,
then rings balar.balarMMIO through the synchronous MMIO bridge.
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
    raise RuntimeError(
        "balarBlock.py not found at {}; expected balar/tests sibling of "
        "quetz/tests".format(BALAR_TESTS_DIR))
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

sst_home = _sst_home()
exe = os.environ.get("QUETZ_EXE", "")
qemu_bin = os.environ.get("QUETZ_QEMU", "")
plugin = os.environ.get(
    "QUETZ_PLUGIN",
    os.path.join(sst_home, "libexec", "libqemu_sst_plugin.so"))
qemu_args = os.environ.get(
    "QUETZ_QEMU_ARGS", "-machine virt -nographic -bios none")
loader = os.environ.get("QUETZ_LOADER", "-kernel")
platform = os.environ.get("QUETZ_PLATFORM", "")

if not exe:
    raise RuntimeError("QUETZ_EXE is not set")
if not qemu_bin:
    raise RuntimeError("QUETZ_QEMU is not set")

balar_mmio_addr = _parse_addr(os.environ.get("BALAR_MMIO_ADDR", "0x70000000"))
balar_mmio_size = _parse_addr(os.environ.get("BALAR_MMIO_SIZE", "0x400"))
dma_mmio_addr = _parse_addr(
    os.environ.get("BALAR_DMA_MMIO_ADDR", hex(balar_mmio_addr + balar_mmio_size)))
dma_mmio_size = _parse_addr(os.environ.get("BALAR_DMA_MMIO_SIZE", "0x200"))
mmio_start = _parse_addr(os.environ.get("QUETZ_MMIO_START", hex(balar_mmio_addr)))
mmio_end = _parse_addr(
    os.environ.get("QUETZ_MMIO_END", hex(dma_mmio_addr + dma_mmio_size - 1)))

# RAM ranges for the coherent fabric. RISC-V virt firmware lives at 0x80000000+
# (see firmware/link_rv64.ld). We MUST NOT advertise the MMIO window through
# the directory/L1/memctrl on the same chiprtr, or MemNIC routing fights with
# balar.balarMMIO and the doorbell never makes it through.
ram_start = _parse_addr(os.environ.get("QUETZ_RAM_START", "0x80000000"))
ram_end = _parse_addr(os.environ.get("QUETZ_RAM_END", "0xFFFFFFFF"))
cfg_file = os.environ.get(
    "BALAR_CONFIG", os.path.join(BALAR_TESTS_DIR, "gpu-v100-mem.cfg"))
cuda_exe = os.environ.get(
    "BALAR_CUDA_EXE_PATH", os.path.join(BALAR_TESTS_DIR, "balar_trace", "vectorAdd"))

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
if platform:
    cpu_params["platform"] = platform

cpu = sst.Component("cpu", "quetz.QuetzComponent")
cpu.addParams(cpu_params)

mmio_rh = cpu.setSubComponent("region_handler", "quetz.MmioForwardRegionHandler", 0)
mmio_rh.addParams({"start": mmio_start, "end": mmio_end})
cpu.enableAllStatistics()

builder = balarBlock.Builder({"BALAR_CUDA_EXE_PATH": cuda_exe})
_, balar_mmio_iface = builder.build(
    cfg_file, balar_mmio_addr, dma_mmio_addr,
    verbosity=int(os.environ.get("BALAR_VERBOSE", "0")))

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
l1_nic.addParams({
    "group": CORE_GROUP,
    "destinations": CORE_DST,
    "network_bw": NETWORK_BW,
})

cpu_mmio_if = cpu.setSubComponent("mmio", "memHierarchy.standardInterface", 0)
cpu_mmio_nic = cpu_mmio_if.setSubComponent("lowlink", "memHierarchy.MemNIC")
cpu_mmio_nic.addParams({
    "group": CORE_GROUP,
    "destinations": CORE_DST,
    "network_bw": NETWORK_BW,
})

balar_mmio_nic = balar_mmio_iface.setSubComponent("lowlink", "memHierarchy.MemNIC")
balar_mmio_nic.addParams({
    "group": MMIO_GROUP,
    "sources": MMIO_SRC,
    "destinations": MMIO_DST,
    "network_bw": NETWORK_BW,
})

dma_mem_nic = dma_mem_if.setSubComponent("lowlink", "memHierarchy.MemNIC")
dma_mem_nic.addParams({
    "group": CORE_GROUP,
    "destinations": CORE_DST,
    "network_bw": NETWORK_BW,
})

dma_mmio_nic = dma_mmio_if.setSubComponent("lowlink", "memHierarchy.MemNIC")
dma_mmio_nic.addParams({
    "group": MEMORY_GROUP,
    "sources": MEMORY_SRC,
    "network_bw": NETWORK_BW,
})

chiprtr = sst.Component("quetz_balar_chiprtr", "merlin.hr_router")
chiprtr.addParams({
    "xbar_bw": "1GB/s",
    "id": "0",
    "input_buf_size": "1KB",
    "num_ports": "6",
    "flit_size": "72B",
    "output_buf_size": "1KB",
    "link_bw": "1GB/s",
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
mem_be.addParams({
    "access_time": "100 ns",
    "mem_size": str(ram_end - ram_start + 1) + "B",
})
mem_hi = memctrl.setSubComponent("highlink", "memHierarchy.MemLink")

directory = sst.Component("directory", "memHierarchy.DirectoryController")
directory.addParams({
    "clock": "1GHz",
    "coherence_protocol": "MESI",
    "cache_line_size": 64,
    "entry_cache_size": 32768,
    "mshr_num_entries": 16,
    "addr_range_start": ram_start,
    "addr_range_end": ram_end,
})
dir_nic = directory.setSubComponent("highlink", "memHierarchy.MemNIC")
dir_nic.addParams({
    "group": MEMORY_GROUP,
    "sources": MEMORY_SRC,
    "network_bw": NETWORK_BW,
})

sst.Link("cpu_l1").connect((cpu, "cache_link_0", "1ns"), (l1_cpu, "port", "1ns"))
sst.Link("mem_bus").connect((mem_hi, "port", "1ns"), (directory, "lowlink", "1ns"))
sst.Link("quetz_l1_rtr").connect((l1_nic, "port", "1ns"), (chiprtr, "port0", "1ns"))
sst.Link("quetz_cpu_mmio_rtr").connect((cpu_mmio_nic, "port", "1ns"), (chiprtr, "port1", "1ns"))
sst.Link("quetz_balar_mmio_rtr").connect((balar_mmio_nic, "port", "1ns"), (chiprtr, "port2", "1ns"))
sst.Link("quetz_dma_mem_rtr").connect((dma_mem_nic, "port", "1ns"), (chiprtr, "port3", "1ns"))
sst.Link("quetz_dma_mmio_rtr").connect((dma_mmio_nic, "port", "1ns"), (chiprtr, "port4", "1ns"))
sst.Link("quetz_dir_rtr").connect((dir_nic, "port", "1ns"), (chiprtr, "port5", "1ns"))

sst.setProgramOption("timebase", "1ps")
sst.setStatisticLoadLevel(4)
sst.setStatisticOutput("sst.statOutputConsole")
