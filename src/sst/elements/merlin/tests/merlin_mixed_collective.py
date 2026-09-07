#!/usr/bin/env python3
# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
"""Deterministic ordinary traffic sharing a fat tree with scalar Allreduce."""

import argparse

import sst
from sst.ember import EmberMPIJob
from sst.merlin.base import EmptyJob, PlatformDefinition, System, hr_router
from sst.merlin.collective import StaticCollectivePlan, StaticCollectiveRouter
from sst.merlin.endpoint import OfferedLoadJob
from sst.merlin.interface import LinkControl, ReorderLinkControl
from sst.merlin.targetgen import ShiftTarget
from sst.merlin.topology import topoFatTree


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--mode", choices=("disabled", "dormant", "active"), default="active")
parser.add_argument("--arbiter", choices=("lru", "rr"), default="lru")
parser.add_argument("--shared-ingress", type=int, choices=(0, 1), default=1)
parser.add_argument("--ingress-bandwidth", type=int, default=1)
parser.add_argument("--ingress-width", type=int, default=1)
parser.add_argument("--queue-depth", type=int, default=1)
parser.add_argument("--load", type=float, default=0.2)
parser.add_argument("--buffer-bytes", type=int, default=128)
parser.add_argument("--packet-bytes", type=int, default=64)
parser.add_argument("--iterations", type=int, default=200)
parser.add_argument("--compute-ns", type=int, default=0)
parser.add_argument("--duration-us", type=int, default=100)
parser.add_argument("--sample-us", type=int, default=5)
parser.add_argument("--statistics", default="mixed-statistics.csv")
parser.add_argument("--motif-log", default="mixed-motifs")
args = parser.parse_args()
if not 0 <= args.load <= 1:
    parser.error("load must be in [0, 1]")
if min(args.ingress_bandwidth, args.ingress_width, args.queue_depth,
       args.iterations, args.duration_us, args.sample_us, args.packet_bytes) < 1:
    parser.error("sizes, iteration counts, bandwidth, and durations must be positive")
if args.compute_ns < 0 or args.buffer_bytes < max(104, args.packet_bytes):
    parser.error("compute must be nonnegative and buffers must hold either packet")

sst.setProgramOption("timebase", "1ps")
# Bound failed progress even though offered_load keeps scheduling traffic after
# marking its own primary component complete. The runner rejects this deadline.
sst.setProgramOption("stop-at", "{}us".format(args.duration_us + 1))
sst.setStatisticLoadLevel(9)
sst.setStatisticOutput("sst.statOutputCSV", {
    "filepath": args.statistics, "outputsimtime": True, "outputrank": False,
})
if args.mode != "disabled" and args.arbiter != "lru":
    parser.error("installed collective processors require the LRU arbiter")

PlatformDefinition.setCurrentPlatform("firefly-defaults")

topology = topoFatTree()
topology.shape = "4,1:2"
topology.routing_alg = "deterministic"
topology.link_latency = "1ns"
topology.host_link_latency = "1ns"


def interface(kind):
    result = kind()
    result.link_bw = "8GB/s"
    result.input_buf_size = "{}B".format(args.buffer_bytes)
    result.output_buf_size = "{}B".format(args.buffer_bytes)
    result.network_service_id = 0
    return result


collective = EmberMPIJob(0, 2)
collective.enableMotifLog(args.motif_log)
collective.network_interface = interface(ReorderLinkControl)
collective.nic.numVNs = 3
collective.addMotif("Init")
if args.mode == "active":
    collective.network_interface.network_service_id = 1
    collective.addMotif("Allreduce iterations={} compute={} count=1 verify=true".format(
        args.iterations, args.compute_ns))
    collective.os.functionsm.Allreduce.enableOffload = True
    collective.os.functionsm.Allreduce.reportOffload = True
collective.addMotif("Fini")

ordinary = OfferedLoadJob(1, 6) if args.load else EmptyJob(1, 6)
ordinary.network_interface = interface(LinkControl)
ordinary.network_interface.enableAllStatistics(
    {"type": "sst.AccumulatorStatistic", "rate": "{}us".format(args.sample_us)})
if args.load:
    ordinary.link_bw = "8GB/s"
    ordinary.message_size = "{}B".format(args.packet_bytes)
    ordinary.offered_load = args.load
    ordinary.warmup_time = "0ns"
    ordinary.collect_time = "{}us".format(args.duration_us)
    ordinary.drain_time = "0ns"
    ordinary.pattern = ShiftTarget()
    ordinary.pattern.shift = 3


def allocate_collective(available, size):
    assert size == 2 and 0 in available and 4 in available
    return [0, 4], [node for node in available if node not in (0, 4)]


System.addAllocationFunction("mixed-collective", allocate_collective)
system = System()
system.setTopology(topology)
system.allocateNodes(collective, "mixed-collective")
system.allocateNodes(ordinary, "linear")
assert collective._nid_map == {0: 0, 4: 1}
assert ordinary._nid_map == {1: 0, 2: 1, 3: 2, 5: 3, 6: 4, 7: 5}

if args.mode == "disabled":
    topology.router = hr_router()
else:
    plan = StaticCollectivePlan.from_fattree(
        topology, logical_ids=collective, reduce_vn=1, result_vn=2)
    topology.router = StaticCollectiveRouter(plan, enable_statistics=True)
    if args.mode == "active":
        collective.useCollectivePlan(plan)

router = topology.router
router.link_bw = "8GB/s"
router.flit_size = "8B"
router.xbar_bw = "8GB/s"
router.input_latency = "0ns"
router.output_latency = "0ns"
router.input_buf_size = "{}B".format(args.buffer_bytes)
router.output_buf_size = "{}B".format(args.buffer_bytes)
router.num_vns = 3
router.xbar_arb = "merlin.xbar_arb_{}".format(args.arbiter)
router.network_service_shared_ingress = bool(args.shared_ingress)
router.network_service_ingress_flits_per_cycle = args.ingress_bandwidth
router.network_service_ingress_width = args.ingress_width
router.network_service_output_queue_depth = args.queue_depth
system.build()
