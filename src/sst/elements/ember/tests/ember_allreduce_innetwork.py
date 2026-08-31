#!/usr/bin/env python3
# Copyright 2009-2026 NTESS. Under the terms of Contract DE-NA0003525 with NTESS,
# the U.S. Government retains certain rights in this software.

import sys

import sst
from sst.ember import *
from sst.merlin.base import *
from sst.merlin.collective import *
from sst.merlin.interface import *
from sst.merlin.topology import *


MODE = sys.argv[1] if len(sys.argv) > 1 else "supported"
if MODE not in ("disabled", "supported", "fallback", "unsupported", "mapped", "missing"):
    raise ValueError("invalid allreduce test mode")

enabled = MODE != "disabled"
mapped = MODE == "mapped"
sst.setProgramOption("timebase", "1ps")
PlatformDefinition.setCurrentPlatform("firefly-defaults")

if enabled:
    endpoint_links = ((0, 3, 0, 0), (1, 0, 0, 1), (2, 1, 1, 0), (3, 2, 1, 1)) \
        if mapped else tuple((nid, nid, nid // 2, nid % 2) for nid in range(4))
    plan = StaticCollectivePlan(
        root_router=2, router_links=((0, 2, 2, 0), (1, 2, 2, 1)),
        endpoint_links=endpoint_links)
    root = plan.endpoint(plan.root_nid)

topology = topoFatTree()
topology.shape = "2,1:2"
topology.routing_alg = "deterministic"
topology.router = StaticCollectiveRouter(plan) if enabled else hr_router()
topology.router.link_bw = topology.router.xbar_bw = "8GB/s"
topology.router.flit_size = "8B"
topology.router.input_latency = topology.router.output_latency = "0ns"
topology.router.input_buf_size = topology.router.output_buf_size = "256B"
topology.router.num_vns = 4 if enabled else 1
topology.router.xbar_arb = "merlin.xbar_arb_rr" if enabled else "merlin.xbar_arb_lru"
topology.link_latency = topology.host_link_latency = "1ns"


networkif = ReorderLinkControl()
networkif.link_bw = "8GB/s"
networkif.input_buf_size = networkif.output_buf_size = "256B"
if enabled:
    networkif.network_service_ids = [] if MODE == "missing" else [1]

job = EmberMPIJob(0, 4)
job.network_interface = networkif
job.addMotif("Init")
iterations = 2 if MODE in ("disabled", "mapped") else 1
count = 4 if MODE == "disabled" else (2 if MODE == "unsupported" else 1)
job.addMotif(f"Allreduce iterations={iterations} compute=0 count={count} verify=true")
job.addMotif("Fini")
job.nic.numVNs = 4 if enabled else 1

if enabled:
    job.nic.getHdrVN = job.nic.getRespSmallVN = job.nic.getRespLargeVN = 3
    job.nic.collectiveEnable = True
    job.nic.collectiveJobNamespace = plan.job_namespace
    job.nic.collectiveRouteId = plan.route_id
    job.nic.collectiveRootNid = root["root_nid"]
    job.nic.collectiveRootLogicalNid = root["root_logical_nid"]
    job.nic.collectiveParticipantSlot = root["participant_slot"]
    job.nic.collectiveReduceVN = root["reduce_vn"]
    job.nic.collectiveResultVN = root["result_vn"]
    job.os.functionsm.smallCollectiveVN = 3
    job.os.functionsm.smallCollectiveSize = 64
    job.os.functionsm.Allreduce.enableOffload = True
    job.os.functionsm.Allreduce.forceSoftware = MODE == "fallback"
    job.os.functionsm.Allreduce.reportOffload = True
    job.os.ctrl.rendezvousVN = job.os.ctrl.ackVN = 3

system = System()
system.setTopology(topology)
system.allocateNodes(job, "random", 2) if mapped else system.allocateNodes(job, "linear")
system.build()

if MODE == "disabled":
    sst.setStatisticOutput("sst.statOutputConsole")
    for statistic in ("sentPkts", "rcvdPkts"):
        sst.enableStatisticForComponentType(
            "firefly.nic", statistic, {"type": "sst.AccumulatorStatistic"})
