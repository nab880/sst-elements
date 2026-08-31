#!/usr/bin/env python3
# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S.
# Government retains certain rights in this software.

import sys

import sst
from sst.ember import *
from sst.merlin.base import *
from sst.merlin.collective import *
from sst.merlin.interface import *
from sst.merlin.topology import *


SERVICE_ID = 1
MODE = sys.argv[1] if len(sys.argv) > 1 else "supported"

if MODE not in ("supported", "fallback", "unsupported"):
    raise ValueError("mode must be supported, fallback, or unsupported")

sst.setProgramOption("timebase", "1ps")
sst.setStatisticOutput("sst.statOutputConsole")

collective_plan = StaticCollectivePlan(
    root_router=2,
    router_links=((0, 2, 2, 0), (1, 2, 2, 1)),
    endpoint_links=tuple((nid, nid, nid // 2, nid % 2) for nid in range(4)),
)
collective_endpoint = collective_plan.endpoint(collective_plan.root_nid)


PlatformDefinition.setCurrentPlatform("firefly-defaults")

topology = topoFatTree()
topology.shape = "2,1:2"
topology.routing_alg = "deterministic"
topology.router = StaticCollectiveRouter(collective_plan, enable_statistics=True)
topology.router.link_bw = "8GB/s"
topology.router.flit_size = "8B"
topology.router.xbar_bw = "8GB/s"
topology.router.input_latency = "0ns"
topology.router.output_latency = "0ns"
topology.router.input_buf_size = "256B"
topology.router.output_buf_size = "256B"
topology.router.num_vns = 4
topology.router.xbar_arb = "merlin.xbar_arb_rr"
topology.link_latency = "1ns"
topology.host_link_latency = "1ns"

networkif = ReorderLinkControl()
networkif.link_bw = "8GB/s"
networkif.input_buf_size = "256B"
networkif.output_buf_size = "256B"
networkif.network_service_ids = [SERVICE_ID]

job = EmberMPIJob(0, 4)
job.network_interface = networkif
job.addMotif("Init")
job.addMotif(
    f"Allreduce iterations=1 compute=0 count={2 if MODE == 'unsupported' else 1} verify=true"
)
job.addMotif("Fini")

job.nic.numVNs = 4
job.nic.getHdrVN = 3
job.nic.getRespSmallVN = 3
job.nic.getRespLargeVN = 3
job.nic.collectiveEnable = True
job.nic.collectiveJobNamespace = collective_plan.job_namespace
job.nic.collectiveRouteId = collective_plan.route_id
job.nic.collectiveRootNid = collective_endpoint["root_nid"]
job.nic.collectiveRootLogicalNid = collective_endpoint["root_logical_nid"]
job.nic.collectiveParticipantSlot = collective_endpoint["participant_slot"]
job.nic.collectiveReduceVN = collective_endpoint["reduce_vn"]
job.nic.collectiveResultVN = collective_endpoint["result_vn"]

job.os.functionsm.smallCollectiveVN = 3
job.os.functionsm.smallCollectiveSize = 64
job.os.functionsm.Allreduce.enableOffload = True
job.os.functionsm.Allreduce.forceSoftware = MODE == "fallback"
job.os.functionsm.Allreduce.reportOffload = True
job.os.ctrl.rendezvousVN = 3
job.os.ctrl.ackVN = 3

system = System()
system.setTopology(topology)
system.allocateNodes(job, "linear")
system.build()

for statistic in (
        "collectiveEnqueued",
        "collectiveSchedulerSends",
        "collectiveSendRetries",
        "collectiveResultsCompleted",
):
    sst.enableStatisticForComponentType(
        "firefly.nic", statistic, {"type": "sst.AccumulatorStatistic"}
    )
