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
if mode not in ("baseline", "memory", "submit", "completion", "contended", "unmodeled", "reentrant") and mode not in invalid_delays:
    raise ValueError("unknown host timing case")
submit_delay, completion_delay = invalid_delays.get(mode, (100 if mode == "submit" else 0, 100 if mode == "completion" else 0))

sst.setProgramOption("timebase", "1ps")
sst.setProgramOption("stop-at", "20us")
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
    node = sst.Component("node%d" % rank, "hg.Node" if mode == "unmodeled" else "hg.NodeCL")
    node.addParams({
        "logicalID": rank, "nranks": 2, "npernode": 1,
        "num_vns": 3, "ordinary_vn": 0, "reduce_vn": 1, "result_vn": 2,
        "num_channels": 1, "channel_bandwidth": "80MB/s" if mode == "memory" else "8GB/s",
    })
    os = node.setSubComponent("os_slot", "hg.OperatingSystem" if mode == "unmodeled" else "hg.OperatingSystemCL")
    os.addParams({
        "app1.name": "collective_host_timing", "app1.exe_library_name": "collective_host_timing",
        "app1.argv": mode,
    })
    nic = node.setSubComponent("nic_slot", "hg.nic")
    nic.addParams({
        "enable_static_collective": True, "physical_endpoint_id": rank, "logical_participant_id": rank,
        "root_nid": 0, "root_logical_nid": 0,
        "collective_submit_delay_ns": submit_delay,
        "collective_completion_delay_ns": completion_delay,
    })
    network = node.setSubComponent("link_control_slot", "merlin.linkcontrol")
    network.addParams({
        "link_bw": "8GB/s", "input_buf_size": "256B", "output_buf_size": "256B",
        "network_service_id": 1,
    })
    link = sst.Link("network_link%d" % rank)
    link.connect((network, "rtr_port", "1ns"), (router, "port%d" % rank, "1ns"))
    link.setNoCut()
