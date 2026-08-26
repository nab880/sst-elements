# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.

import sst


sst.setProgramOption("timebase", "1ps")

router = sst.Component("router", "merlin.hr_router")
router.addParams({
    "id": 0,
    "num_ports": 1,
    "num_vns": 1,
    "link_bw": "8GB/s",
    "flit_size": "8B",
    "xbar_bw": "8GB/s",
    "input_latency": "0ns",
    "output_latency": "0ns",
    "input_buf_size": "8B",
    "output_buf_size": "8B",
})
router.setSubComponent("topology", "merlin.singlerouter")

endpoint = sst.Component(
    "endpoint",
    "merlin.network_service_missing_processor_endpoint",
)
network_if = endpoint.setSubComponent("networkIF", "merlin.linkcontrol")
network_if.addParams({
    "link_bw": "8GB/s",
    "input_buf_size": "8B",
    "output_buf_size": "8B",
    "network_service_ids": [0x8000],
})

link = sst.Link("endpoint_link")
link.connect(
    (network_if, "rtr_port", "1ns"),
    (router, "port0", "1ns"),
)
link.setNoCut()
