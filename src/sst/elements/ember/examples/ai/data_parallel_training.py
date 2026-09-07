#!/usr/bin/env python3
# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights
# in this software.

"""Run a functional data-parallel SGD workload on the static collective tree."""

import sys

import sst
from sst.ember import *
from sst.merlin.base import *
from sst.merlin.collective import *
from sst.merlin.interface import *
from sst.merlin.topology import *


SCENARIO = sys.argv[1] if len(sys.argv) > 1 else "scalar"
PATH = sys.argv[2] if len(sys.argv) > 2 else "offload"
EPOCHS = int(sys.argv[3]) if len(sys.argv) > 3 else 8
COMPUTE_NS = int(sys.argv[4]) if len(sys.argv) > 4 else 500
REPORT_PATH = len(sys.argv) > 5 and sys.argv[5] == "report"

if SCENARIO not in ("scalar", "hybrid"):
    raise ValueError("scenario must be scalar or hybrid")
if PATH not in ("software", "offload"):
    raise ValueError("path must be software or offload")
if EPOCHS <= 0 or COMPUTE_NS < 0:
    raise ValueError("epochs must be positive and compute time must be nonnegative")
if len(sys.argv) > 6 or (len(sys.argv) == 6 and sys.argv[5] != "report"):
    raise ValueError("the optional fifth model argument must be report")

RANKS = 8
PARAMETERS = 1 if SCENARIO == "scalar" else 4
SYNC_LOSS = SCENARIO == "hybrid"
SERVICE_ID = 1

sst.setProgramOption("timebase", "1ps")
sst.setStatisticOutput("sst.statOutputConsole")

PlatformDefinition.setCurrentPlatform("firefly-defaults")

topology = topoFatTree()
topology.shape = "2,1:2,1:2"
topology.routing_alg = "deterministic"
topology.link_latency = "20ns"
topology.host_link_latency = "20ns"

networkif = ReorderLinkControl()
networkif.link_bw = "8GB/s"
networkif.input_buf_size = "4kB"
networkif.output_buf_size = "4kB"
networkif.network_service_id = SERVICE_ID

job = EmberMPIJob(0, RANKS)
job.network_interface = networkif
job.addMotif("Init")
job.addMotif(
    f"AITraining epochs={EPOCHS} compute={COMPUTE_NS} "
    f"parameters={PARAMETERS} learning_rate=0.5 "
    f"sync_loss={'true' if SYNC_LOSS else 'false'} verify=true"
)
job.addMotif("Fini")

# Ordinary Firefly traffic stays on VN 0; the collective tree owns VNs 1
# and 2 on every router it spans.
job.nic.numVNs = 3
job.os.functionsm.Allreduce.enableOffload = True
job.os.functionsm.Allreduce.forceSoftware = PATH == "software"
job.os.functionsm.Allreduce.reportOffload = REPORT_PATH

system = System()
system.setTopology(topology)
system.allocateNodes(job, "linear")

plan = StaticCollectivePlan.from_fattree(topology, logical_ids=job, reduce_vn=1, result_vn=2)
job.useCollectivePlan(plan)
topology.router = StaticCollectiveRouter(plan, enable_statistics=True)
topology.router.link_bw = "8GB/s"
topology.router.flit_size = "8B"
topology.router.xbar_bw = "8GB/s"
topology.router.input_latency = "20ns"
topology.router.output_latency = "20ns"
topology.router.input_buf_size = "4kB"
topology.router.output_buf_size = "4kB"
topology.router.num_vns = 3
topology.router.xbar_arb = "merlin.xbar_arb_lru"
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

print(
    f"AI training demo scenario={SCENARIO} path={PATH} "
    f"ranks={RANKS} epochs={EPOCHS} compute_ns={COMPUTE_NS}"
)
