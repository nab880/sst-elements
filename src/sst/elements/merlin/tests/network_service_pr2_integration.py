# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.

import sst


SERVICE_ID = 0x8000

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
    "input_buf_size": "8B",
    "output_buf_size": "8B",
    "xbar_arb": "merlin.xbar_arb_rr",
    "network_service_output_queue_depth": 2,
})
router.setSubComponent("topology", "merlin.singlerouter")
processor = router.setSubComponent("network_service", "merlin.network_service_pr2_processor")

trigger_endpoint = None

for endpoint_id in range(2):
    endpoint = sst.Component(
        "endpoint%d" % endpoint_id,
        "merlin.network_service_pr2_endpoint",
    )
    endpoint.addParam("id", endpoint_id)
    if endpoint_id == 0:
        trigger_endpoint = endpoint

    network_if = endpoint.setSubComponent("networkIF", "merlin.linkcontrol")
    network_if.addParams({
        "link_bw": "8GB/s",
        "input_buf_size": "8B",
        "output_buf_size": "8B",
        "network_service_ids": [SERVICE_ID],
    })

    link = sst.Link("endpoint%d_link" % endpoint_id)
    link.connect(
        (network_if, "rtr_port", "1ns"),
        (router, "port%d" % endpoint_id, "1ns"),
    )
    link.setNoCut()

trigger = sst.Link("network_service_external_trigger")
trigger.connect(
    (trigger_endpoint, "service_trigger", "1ns"),
    (processor, "trigger", "1ns"),
)
trigger.setNoCut()
