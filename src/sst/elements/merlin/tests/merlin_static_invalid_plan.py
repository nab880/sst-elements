# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.

from sst.merlin.base import System
from sst.merlin.collective import StaticCollectivePlan, StaticCollectiveRouter
from sst.merlin.endpoint import TestJob
from sst.merlin.interface import ReorderLinkControl
from sst.merlin.topology import topoFatTree


ENDPOINT_LINKS = tuple((nid, nid, nid // 2, nid % 2) for nid in range(4))

plan = StaticCollectivePlan(
    root_router=2,
    router_links=((0, 2, 2, 1), (1, 2, 2, 0)),
    endpoint_links=ENDPOINT_LINKS,
)

topology = topoFatTree()
topology.shape = "2,1:2"
topology.routing_alg = "deterministic"
topology.router = StaticCollectiveRouter(plan)
topology.router.link_bw = "8GB/s"
topology.router.flit_size = "8B"
topology.router.xbar_bw = "8GB/s"
topology.router.input_latency = "0ns"
topology.router.output_latency = "0ns"
topology.router.input_buf_size = "256B"
topology.router.output_buf_size = "256B"
topology.router.num_vns = 3
topology.link_latency = "1ns"
topology.host_link_latency = "1ns"

network_if = ReorderLinkControl()
network_if.link_bw = "8GB/s"
network_if.input_buf_size = "256B"
network_if.output_buf_size = "256B"

job = TestJob(0, 4)
job.network_interface = network_if
job.num_messages = 1
job.message_size = "8B"

system = System()
system.setTopology(topology)
system.allocateNodes(job, "linear")
system.build()

raise RuntimeError("invalid static collective plan reached timed simulation")
