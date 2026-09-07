#!/usr/bin/env python3
# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.

import sys

import sst
from sst.merlin.base import *
from sst.merlin.collective import *
from sst.merlin.interface import *
from sst.merlin.topology import *
from sst.hg import *


SERVICE_ID = 1
MODE = sys.argv[1] if len(sys.argv) > 1 else "supported"

if MODE not in ("supported", "fallback", "sparse", "missing-service", "remapped-service"):
    raise ValueError("unknown in-network allreduce mode")

sst.setProgramOption("timebase", "1ps")
if len(sys.argv) > 3:
    raise ValueError("unexpected model arguments")
if len(sys.argv) > 2:
    sst.setStatisticOutput("sst.statOutputCSV", {
        "filepath": sys.argv[2], "outputrank": True,
    })
else:
    sst.setStatisticOutput("sst.statOutputConsole")

topology = topoFatTree()
topology.shape = "2,1:2,1:2" if MODE == "sparse" else "2,1:2"
topology.routing_alg = "deterministic"
topology.link_latency = "1ns"
topology.host_link_latency = "1ns"


platform = PlatformDefinition("platform_mask_mpi_innetwork")
PlatformDefinition.registerPlatformDefinition(platform)
PlatformDefinition.setCurrentPlatform("platform_mask_mpi_innetwork")

platform.addParamSet("node", {
    "verbose": "0",
    "name": "hg.NodeCL",
    "negligible_compute_bytes": "64B",
    "parallelism": "1.0",
    "frequency": "2.1GHz",
    "flow_mtu": "512",
    "channel_bandwidth": "11.2 GB/s",
    "num_channels": "4",
    # Native application and manager traffic intentionally share VN 0; the
    # collective plan below claims VNs 1 and 2.
    "num_vns": 3,
    "ordinary_vn": 0,
    "manager_vn": 0,
})

platform.addParamSet("nic", {
    "verbose": "0",
    "mtu": "4096 B",
})

platform.addParamSet("operating_system", {
    "verbose": "0",
    "name": "hg.OperatingSystemCL",
    "ncores": "1",
    "nsockets": "1",
    "app1.name": "allreduce_innetwork",
    "app1.exe_library_name": "allreduce_innetwork",
    "app1.argv": "fallback" if MODE == "fallback" else "supported",
    "app1.dependencies": ["sumi"],
    "app1.libraries": [
        "computelibrary:ComputeLibrary",
        "mask_mpi:MpiApi",
    ],
    "app1.post_rdma_delay": "1.5us",
    "app1.post_header_delay": "0.5us",
    "app1.poll_delay": "0us",
    "app1.rdma_pin_latency": "5.43us",
    "app1.rdma_page_delay": "50.50ns",
    "app1.rdma_page_size": "4096",
    "app1.max_vshort_msg_size": "4096 B",
    "app1.max_eager_msg_size": "32768 B",
    "app1.use_put_window": "false",
    "app1.compute_library_access_width": "64",
    "app1.compute_library_loop_overhead": "1.0",
    "app1.enable_collective_offload": True,
})

platform.addParamSet("network_interface", {
    "link_bw": "8GB/s",
    "input_buf_size": "256B",
    "output_buf_size": "256B",
    # Unallocated hosts and ordinary jobs inherit this platform default.
    "network_service_id": 0,
})
platform.addClassType(
    "network_interface", "sst.merlin.interface.ReorderLinkControl"
)

platform.addParamSet("router", {
    "link_bw": "8GB/s",
    "flit_size": "8B",
    "xbar_bw": "8GB/s",
    "input_latency": "0ns",
    "output_latency": "0ns",
    "input_buf_size": "256B",
    "output_buf_size": "256B",
    "num_vns": 3,
    "xbar_arb": "merlin.xbar_arb_lru",
})

platform.addParamSet("topology", {
    "shape": topology.shape,
    "routing_alg": "deterministic",
})

job = HgJob(0, 4)
job.network_interface.network_service_id = 0 if MODE == "missing-service" else SERVICE_ID
if MODE == "remapped-service":
    job.network_interface.vn_remap = [0, 2, 1]
system = System()
system.setTopology(topology)
if MODE == "sparse":
    # Leave entire leaf routers outside the collective plan; System.build()
    # fills their unused hosts with ordinary EmptyJob interfaces.
    system.allocateNodes(job, "linear")
else:
    system.allocateNodes(job, "random", 7)

# The collective tree follows the built fat tree and the job's allocation.
collective_plan = StaticCollectivePlan.from_fattree(
    topology, logical_ids=job, reduce_vn=1, result_vn=2)
job.useCollectivePlan(collective_plan)
topology.router = StaticCollectiveRouter(collective_plan, enable_statistics=True)
system.build()
