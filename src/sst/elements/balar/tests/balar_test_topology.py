"""
Shared SST topology builder for Balar contract tests.
"""

import sst

DEBUG_PARAMS = {"debug": 0, "debug_level": 10}
NETWORK_BW = "25GB/s"
CLOCK = "2GHz"

CORE_GROUP = 0
MMIO_GROUP = 1
MEMORY_GROUP = 2

CORE_DST = [MEMORY_GROUP, MMIO_GROUP]
MMIO_SRC = [CORE_GROUP]
MMIO_DST = [MEMORY_GROUP]
MEMORY_SRC = [CORE_GROUP, MMIO_GROUP]


def build_testcpu_router(balar_builder, cfg_file, balar_verbosity=0, dma_verbosity=0):
    """Return (cpu, balar_mmio_addr) for merlin router + BalarTestCPU."""
    balar_mmio_iface, balar_mmio_addr, dma_mem_if, dma_mmio_if = balar_builder.buildTestCPU(
        cfg_file, balar_verbosity, dma_verbosity
    )

    mmio_nic = balar_mmio_iface.setSubComponent("lowlink", "memHierarchy.MemNIC")
    mmio_nic.addParams({
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

    cpu = sst.Component("cpu", "balar.BalarTestCPU")
    cpu.addParams({
        "clock": CLOCK,
        "verbose": balar_verbosity,
        "scratch_mem_addr": 0,
        "gpu_addr": balar_mmio_addr,
        "enable_memcpy_dump": False,
    })
    iface = cpu.setSubComponent("memory", "memHierarchy.standardInterface")
    iface.addParams(DEBUG_PARAMS)
    cpu_nic = iface.setSubComponent("lowlink", "memHierarchy.MemNIC")
    cpu_nic.addParams({
        "group": CORE_GROUP,
        "destinations": CORE_DST,
        "network_bw": NETWORK_BW,
    })

    chiprtr = sst.Component("chiprtr", "merlin.hr_router")
    chiprtr.addParams({
        "xbar_bw": "1GB/s",
        "id": "0",
        "input_buf_size": "1KB",
        "num_ports": "5",
        "flit_size": "72B",
        "output_buf_size": "1KB",
        "link_bw": "1GB/s",
        "topology": "merlin.singlerouter",
    })
    chiprtr.setSubComponent("topology", "merlin.singlerouter")

    memctrl = sst.Component("memory", "memHierarchy.MemController")
    memctrl.addParams({
        "clock": "1GHz",
        "addr_range_end": balar_mmio_addr - 1,
    })
    mem_nic = memctrl.setSubComponent("highlink", "memHierarchy.MemNIC")
    mem_nic.addParams({
        "group": MEMORY_GROUP,
        "sources": MEMORY_SRC,
        "network_bw": NETWORK_BW,
    })
    memory = memctrl.setSubComponent("backend", "memHierarchy.simpleMem")
    memory.addParams({
        "access_time": "100 ns",
        "mem_size": "512MiB",
    })

    sst.Link("link_cpu").connect((cpu_nic, "port", "1000ps"), (chiprtr, "port0", "1000ps"))
    sst.Link("link_mmio").connect((mmio_nic, "port", "500ps"), (chiprtr, "port1", "500ps"))
    sst.Link("link_dma_mem").connect((dma_mem_nic, "port", "500ps"), (chiprtr, "port2", "500ps"))
    sst.Link("link_dma_mmio").connect((dma_mmio_nic, "port", "500ps"), (chiprtr, "port3", "500ps"))
    sst.Link("link_mem").connect((mem_nic, "port", "1000ps"), (chiprtr, "port4", "1000ps"))

    return cpu, balar_mmio_addr
