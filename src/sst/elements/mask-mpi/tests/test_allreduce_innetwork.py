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

if MODE not in ("supported", "fallback", "missing-service"):
    raise ValueError("mode must be supported, fallback, or missing-service")

sst.setProgramOption("timebase", "1ps")
sst.setStatisticOutput("sst.statOutputConsole")

collective_plan = StaticCollectivePlan(
    root_router=2,
    router_links=((0, 2, 2, 0), (1, 2, 2, 1)),
    endpoint_links=(
        (0, 2, 0, 0),
        (1, 1, 0, 1),
        (2, 3, 1, 0),
        (3, 0, 1, 1),
    ),
)
collective_endpoint = collective_plan.endpoint(collective_plan.root_nid)


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
    # Native application and manager traffic intentionally share VN 2.
    "num_vns": 3,
    "ordinary_vn": 2,
    "manager_vn": 2,
    "reduce_vn": collective_endpoint["reduce_vn"],
    "result_vn": collective_endpoint["result_vn"],
})

platform.addParamSet("nic", {
    "verbose": "0",
    "mtu": "4096 B",
    "enable_static_collective": True,
    "job_namespace": collective_plan.job_namespace,
    "route_id": collective_plan.route_id,
    "root_nid": collective_endpoint["root_nid"],
    "root_logical_nid": collective_endpoint["root_logical_nid"],
    "participant_slot": collective_endpoint["participant_slot"],
})

platform.addParamSet("operating_system", {
    "verbose": "0",
    "name": "hg.OperatingSystemCL",
    "ncores": "1",
    "nsockets": "1",
    "app1.name": "allreduce_innetwork",
    "app1.exe_library_name": "allreduce_innetwork",
    "app1.argv": MODE,
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
    "network_service_ids": [] if MODE == "missing-service" else [SERVICE_ID],
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
    "xbar_arb": "merlin.xbar_arb_rr",
})

platform.addParamSet("topology", {
    "shape": "2,1:2",
    "routing_alg": "deterministic",
})

topology = topoFatTree()
topology.router = StaticCollectiveRouter(collective_plan, enable_statistics=True)
topology.link_latency = "1ns"
topology.host_link_latency = "1ns"

job = HgJob(0, 4)
system = System()
system.setTopology(topology)
system.allocateNodes(job, "random", 7)
system.build()
