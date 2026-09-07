#!/usr/bin/env python3
# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.

import sys

import sst
from sst.ember import EmberMPIJob
from sst.merlin.base import PlatformDefinition, System
from sst.merlin.collective import StaticCollectivePlan, StaticCollectiveRouter
from sst.merlin.interface import ReorderLinkControl
from sst.merlin.topology import topoFatTree


mode = sys.argv[1] if len(sys.argv) > 1 else "supported"
if mode not in ("supported", "physical-mismatch", "logical-mismatch",
                "missing-rank", "reallocated", "reordered"):
    raise ValueError("unknown sparse collective test mode")

sst.setProgramOption("timebase", "1ps")
if len(sys.argv) > 3:
    raise ValueError("unexpected model arguments")
if len(sys.argv) > 2:
    sst.setStatisticOutput("sst.statOutputCSV", {
        "filepath": sys.argv[2], "outputrank": True,
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
networkif.network_service_id = 1

job = EmberMPIJob(0, 2)
job.network_interface = networkif
job.addMotif("Init")
job.addMotif("Allreduce iterations=2 compute=0 count=1 verify=true")
job.addMotif("Fini")
job.nic.numVNs = 3
job.os.functionsm.Allreduce.enableOffload = True
job.os.functionsm.Allreduce.reportOffload = True


def allocate_sparse(available, size):
    assert size == 2 and all(node in available for node in (3, 1))
    return [3, 1], [node for node in available if node not in (3, 1)]


System.addAllocationFunction("collective-sparse-test", allocate_sparse)
system = System()
system.setTopology(topology)
system.allocateNodes(job, "collective-sparse-test")
assert job._nid_map == {3: 0, 1: 1}

membership = job
if mode == "physical-mismatch":
    membership = {0: 0, 2: 1}
elif mode == "logical-mismatch":
    membership = {3: 1, 1: 0}
elif mode == "missing-rank":
    job._nid_map = {3: 0}
plan = StaticCollectivePlan.from_fattree(
    topology, logical_ids=membership, reduce_vn=1, result_vn=2)
job.useCollectivePlan(plan)
assert plan.endpoint_ids == (1, 3)
if mode == "reallocated":
    job._nid_map = {0: 0, 2: 1}
elif mode == "reordered":
    job._nid_map = {3: 1, 1: 0}

topology.router = StaticCollectiveRouter(plan, enable_statistics=True)
topology.router.link_bw = "8GB/s"
topology.router.flit_size = "8B"
topology.router.xbar_bw = "8GB/s"
topology.router.input_latency = "0ns"
topology.router.output_latency = "0ns"
topology.router.input_buf_size = "256B"
topology.router.output_buf_size = "256B"
topology.router.num_vns = 3


class SparseEndpoints:
    def build(self, node_id, extra_keys):
        # Leave unused physical ports disconnected, rather than filling gaps
        # with dummy NICs, so accidental host-port compaction is observable.
        if node_id not in job._nid_map:
            return None, None
        return job.build(node_id, extra_keys)


topology.build(SparseEndpoints())
