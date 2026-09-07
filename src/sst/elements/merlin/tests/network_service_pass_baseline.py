# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.

import sys

import sst


service_enabled = len(sys.argv) > 1 and sys.argv[1] == "pass"

sst.setProgramOption("timebase", "1ps")

router = sst.Component("router", "merlin.hr_router")
router.addParams({
    "id": 0,
    "num_ports": 2,
    "num_vns": 1,
    "link_bw": "8GB/s",
    "flit_size": "8B",
    "xbar_bw": "8GB/s",
    "input_latency": "0ns",
    "output_latency": "0ns",
    "input_buf_size": "64B",
    "output_buf_size": "64B",
    "xbar_arb": "merlin.xbar_arb_rr",
})
router.setSubComponent("topology", "merlin.singlerouter")
if service_enabled:
    processor = router.setSubComponent("network_service", "merlin.network_service_pass")
    processor.addParam("service_id", 0x8000)

for endpoint_id in range(2):
    endpoint = sst.Component("endpoint%d" % endpoint_id, "merlin.test_nic")
    endpoint.addParams({
        "id": endpoint_id,
        "num_peers": 2,
        "num_messages": 8,
        "message_size": "64b",
    })
    network_if = endpoint.setSubComponent("networkIF", "merlin.linkcontrol")
    network_if.addParams({
        "link_bw": "8GB/s",
        "input_buf_size": "64B",
        "output_buf_size": "64B",
    })
    link = sst.Link("endpoint%d_link" % endpoint_id)
    link.connect(
        (network_if, "rtr_port", "1ns"),
        (router, "port%d" % endpoint_id, "1ns"),
    )
    link.setNoCut()
