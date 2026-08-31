#!/usr/bin/env python3
# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.

import sys

import sst
from sst.merlin.base import *
from sst.merlin.collective import *
from sst.merlin.topology import *
from sst.hg import *


MODE = sys.argv[1] if len(sys.argv) > 1 else "active"
if MODE not in ("active", "missing-service"):
    raise ValueError("mode must be active or missing-service")

sst.setProgramOption("timebase", "1ps")
sst.setStatisticOutput("sst.statOutputConsole")

plan = StaticCollectivePlan(
    root_router=2,
    router_links=((0, 2, 2, 0), (1, 2, 2, 1)),
    endpoint_links=((0, 2, 0, 0), (1, 1, 0, 1),
                    (2, 3, 1, 0), (3, 0, 1, 1)),
)
endpoint = plan.endpoint(plan.root_nid)

PlatformDefinition.loadPlatformFile("platform_file_mask_mpi_test")
PlatformDefinition.compose(
    "platform_mask_mpi_innetwork", [("platform_mask_mpi_test", "ALL")])
PlatformDefinition.setCurrentPlatform("platform_mask_mpi_innetwork")
platform = PlatformDefinition.getCurrentPlatform()
platform.addClassType("network_interface", "sst.merlin.interface.ReorderLinkControl")
platform.addParamSet("node", {
    "num_vns": 3, "ordinary_vn": 2, "manager_vn": 2,
    "reduce_vn": endpoint["reduce_vn"], "result_vn": endpoint["result_vn"],
})
platform.addParamSet("nic", {
    "enable_static_collective": True,
    "job_namespace": plan.job_namespace,
    "route_id": plan.route_id,
    "root_nid": endpoint["root_nid"],
    "root_logical_nid": endpoint["root_logical_nid"],
    "participant_slot": endpoint["participant_slot"],
})
platform.addParamSet("operating_system", {
    "ncores": 1, "nsockets": 1,
    "app1.name": "allreduce_innetwork",
    "app1.exe_library_name": "allreduce_innetwork",
    "app1.dependencies": ["sumi"],
    "app1.libraries": ["computelibrary:ComputeLibrary", "mask_mpi:MpiApi"],
    "app1.enable_collective_offload": True,
})
platform.addParamSet("network_interface", {
    "link_bw": "8GB/s", "input_buf_size": "256B", "output_buf_size": "256B",
    "network_service_ids": [] if MODE == "missing-service" else [1],
})
platform.addParamSet("router", {
    "link_bw": "8GB/s", "xbar_bw": "8GB/s",
    "input_latency": "0ns", "output_latency": "0ns",
    "input_buf_size": "256B", "output_buf_size": "256B",
    "num_vns": 3, "xbar_arb": "merlin.xbar_arb_rr",
})
platform.addParamSet("topology", {"shape": "2,1:2", "routing_alg": "deterministic"})

topology = topoFatTree()
topology.router = StaticCollectiveRouter(plan, enable_statistics=True)
topology.link_latency = topology.host_link_latency = "1ns"

system = System()
system.setTopology(topology)
system.allocateNodes(HgJob(0, 4), "random", 7)
system.build()
