# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.

import sys

import sst
from sst.merlin.collective import StaticCollectivePlan


mode = sys.argv[1] if len(sys.argv) > 1 else "baseline"
invalid_delays = {
    "negative_submit": (-1, 0), "negative_completion": (0, -1),
    "overflow_submit": (2**63 - 1, 0), "overflow_completion": (0, 2**63 - 1),
}
if mode not in ("baseline", "read", "write_baseline", "write", "submit", "completion") and mode not in invalid_delays:
    raise ValueError("unknown host timing case")
submit_delay, completion_delay = invalid_delays.get(mode, (100 if mode == "submit" else 0, 100 if mode == "completion" else 0))

sst.setProgramOption("timebase", "1ps")
sst.setProgramOption("stop-at", "20us")
if len(sys.argv) > 3:
    raise ValueError("unexpected model arguments")
if len(sys.argv) > 2:
    sst.setStatisticOutput("sst.statOutputCSV", {
        "filepath": sys.argv[2], "outputrank": True,
    })
else:
    sst.setStatisticOutput("sst.statOutputConsole")
plan = StaticCollectivePlan(0, [], [(0, 0, 0, 0), (1, 1, 0, 1)], reduce_vn=1, result_vn=2)

router = sst.Component("router", "merlin.hr_router")
router.addParams({
    "id": 0, "num_ports": 2, "num_vns": 3,
    "link_bw": "8GB/s", "flit_size": "8B", "xbar_bw": "8GB/s",
    "input_latency": "0ns", "output_latency": "0ns",
    "input_buf_size": "256B", "output_buf_size": "256B",
    "xbar_arb": "merlin.xbar_arb_lru", "network_service_output_queue_depth": 1,
})
router.setSubComponent("topology", "merlin.singlerouter")
router.setSubComponent("network_service", "merlin.collective_static_processor").addParams(plan.processor_params(0))

for rank in range(2):
    host = sst.Component("host%d" % rank, "firefly.collective_host_timing_test")
    host.addParam("rank", rank)
    vnic = host.setSubComponent("virtNic", "firefly.VirtNic")
    nic = sst.Component("nic%d" % rank, "firefly.nic")
    nic.addParams({
        "nid": rank, "num_vNics": 1, "numVNs": 3, "packetSize": "256B", "nic2host_lat": "1ns",
        "collectiveEnable": True, "collectiveParticipantLogicalId": rank,
        "collectiveRootNid": 0, "collectiveRootLogicalNid": 0,
        "collectiveReduceVN": 1, "collectiveResultVN": 2,
        "collectiveSubmitDelay_ns": submit_delay,
        "collectiveCompletionDelay_ns": completion_delay,
        "useSimpleMemoryModel": 1,
        "simpleMemoryModel.useHostCache": "no",
        "simpleMemoryModel.useBusBridge": "no",
        "simpleMemoryModel.memNumSlots": 1 if mode in ("write_baseline", "write") else 10,
        "simpleMemoryModel.memReadLat_ns": 110 if mode == "read" else 10,
        "simpleMemoryModel.memWriteLat_ns": 110 if mode == "write" else 10,
    })
    nic.enableStatistics([
        "collectiveDmaReadBytes", "collectiveDmaWriteBytes", "collectiveResultsCompleted",
    ], {"type": "sst.AccumulatorStatistic"})
    network = nic.setSubComponent("rtrLink", "merlin.linkcontrol")
    network.addParams({
        "link_bw": "8GB/s", "input_buf_size": "256B", "output_buf_size": "256B",
        "network_service_id": 1,
    })
    host_link = sst.Link("host_link%d" % rank)
    host_link.connect((vnic, "nic", "1ns"), (nic, "core0", "1ns"))
    host_link.setNoCut()
    network_link = sst.Link("network_link%d" % rank)
    network_link.connect((network, "rtr_port", "1ns"), (router, "port%d" % rank, "1ns"))
