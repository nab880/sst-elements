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
PROFILE = sys.argv[2] if len(sys.argv) > 2 else "default"
if PROFILE not in ("default", "flit4", "flit16", "fast-ingress", "wide-ingress",
                   "private-ingress", "slow-reduction", "zero-width", "zero-bandwidth",
                   "identity-vns", "remapped-vns"):
    raise ValueError("unknown network profile")

if MODE not in ("supported", "fallback", "unsupported"):
    raise ValueError("mode must be supported, fallback, or unsupported")

sst.setProgramOption("timebase", "1ps")
if len(sys.argv) > 4:
    raise ValueError("unexpected model arguments")
if len(sys.argv) > 3:
    sst.setStatisticOutput("sst.statOutputCSV", {
        "filepath": sys.argv[3], "outputrank": True,
    })
else:
    sst.setStatisticOutput("sst.statOutputConsole")

PlatformDefinition.setCurrentPlatform("firefly-defaults")

topology = topoFatTree()
topology.shape = "2,1:2"
topology.routing_alg = "deterministic"
topology.link_latency = "1ns"
topology.host_link_latency = "1ns"

networkif = ReorderLinkControl()
networkif.link_bw = "8GB/s"
networkif.input_buf_size = "256B"
networkif.output_buf_size = "256B"
networkif.network_service_id = SERVICE_ID
if PROFILE in ("identity-vns", "remapped-vns"):
    networkif.vn_remap = [0, 1, 2] if PROFILE == "identity-vns" else [0, 2, 1]

job = EmberMPIJob(0, 4)
job.network_interface = networkif
job.addMotif("Init")
job.addMotif(
    f"Allreduce iterations=1 compute=0 count={2 if MODE == 'unsupported' else 1} verify=true"
)
job.addMotif("Fini")

# Ordinary Firefly traffic stays on VN 0; the collective tree owns VNs 1
# and 2 on every router it spans.
job.nic.numVNs = 3
job.os.functionsm.Allreduce.enableOffload = True
job.os.functionsm.Allreduce.forceSoftware = MODE == "fallback"
job.os.functionsm.Allreduce.reportOffload = True

system = System()
system.setTopology(topology)
system.allocateNodes(job, "linear")

# The collective tree follows the built fat tree and the job's allocation.
collective_plan = StaticCollectivePlan.from_fattree(
    topology, logical_ids=job, reduce_vn=1, result_vn=2,
    reduction_latency_cycles=31 if PROFILE == "slow-reduction" else 1)
job.useCollectivePlan(collective_plan)
topology.router = StaticCollectiveRouter(collective_plan, enable_statistics=True)
topology.router.link_bw = "8GB/s"
topology.router.flit_size = {"flit4": "4B", "flit16": "16B"}.get(PROFILE, "8B")
topology.router.xbar_bw = "8GB/s"
topology.router.input_latency = "0ns"
topology.router.output_latency = "0ns"
topology.router.input_buf_size = "256B"
topology.router.output_buf_size = "256B"
topology.router.num_vns = 3
topology.router.xbar_arb = "merlin.xbar_arb_lru"

# Resource timing is independent of the scalar collective signature.
topology.router.network_service_ingress_width = 0 if PROFILE == "zero-width" else (4 if PROFILE == "wide-ingress" else 1)
topology.router.network_service_ingress_flits_per_cycle = 0 if PROFILE == "zero-bandwidth" else (13 if PROFILE == "fast-ingress" else 1)
topology.router.network_service_shared_ingress = PROFILE != "private-ingress"
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
