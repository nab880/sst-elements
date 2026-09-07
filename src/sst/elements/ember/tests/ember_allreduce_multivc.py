# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
"""Exercise host VN credits and router VC credits with two VCs per VN."""

import sys

import sst
from sst.ember import EmberMPIJob
from sst.merlin.base import PlatformDefinition, System
from sst.merlin.collective import StaticCollectivePlan, StaticCollectiveRouter
from sst.merlin.interface import ReorderLinkControl
from sst.merlin.topology import topoTorus


mode = sys.argv[1] if len(sys.argv) > 1 else "host"
if mode not in ("host", "host-swapped", "routed", "routed-swapped"):
    raise ValueError("unknown multi-VC collective test mode")
if len(sys.argv) > 3:
    raise ValueError("expected: mode [statistics-file]")
statistics_file = sys.argv[2] if len(sys.argv) > 2 else None
routed = mode.startswith("routed")
swapped = mode.endswith("swapped")
reduce_vn, result_vn = (2, 1) if swapped else (1, 2)

sst.setProgramOption("timebase", "1ps")
sst.setProgramOption("stop-at", "20us")
if statistics_file:
    sst.setStatisticOutput("sst.statOutputCSV", {
        "filepath": statistics_file,
        "outputrank": True,
    })
else:
    sst.setStatisticOutput("sst.statOutputConsole")
PlatformDefinition.setCurrentPlatform("firefly-defaults")

# Torus has two VCs per VN. A one-router torus tests host capacity alone;
# a two-router torus additionally sends contributions/results across R2R links.
topology = topoTorus()
topology.shape = "2" if routed else "1"
topology.width = "1"
topology.local_ports = 1 if routed else 2
topology.link_latency = "1ns"
topology.host_link_latency = "1ns"

networkif = ReorderLinkControl()
networkif.link_bw = "8GB/s"
networkif.input_buf_size = "104B"
networkif.output_buf_size = "104B"
networkif.network_service_id = 1

job = EmberMPIJob(0, 2)
job.network_interface = networkif
job.nic.numVNs = 3
job.addMotif("Init")
job.addMotif("Allreduce iterations=4 compute=0 count=1 verify=true")
job.addMotif("Fini")
job.os.functionsm.Allreduce.enableOffload = True
job.os.functionsm.Allreduce.reportOffload = True

system = System()
system.setTopology(topology)
system.allocateNodes(job, "linear")
router_links = [(0, 0, 1, 1)] if routed else []
endpoint_links = [(0, 0, 0, 2), (1, 1, 1, 2)] if routed else [
    (0, 0, 0, 2), (1, 1, 0, 3)]
plan = StaticCollectivePlan(0, router_links, endpoint_links,
                          reduce_vn=reduce_vn, result_vn=result_vn)
job.useCollectivePlan(plan)
topology.router = StaticCollectiveRouter(plan, enable_statistics=True)
topology.router.link_bw = "8GB/s"
topology.router.flit_size = "8B"
topology.router.xbar_bw = "8GB/s"
topology.router.input_latency = "0ns"
topology.router.output_latency = "0ns"
topology.router.input_buf_size = "104B"
topology.router.output_buf_size = "104B"
topology.router.num_vns = 3
system.build()
