# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.

import sys
import sst

MODE = sys.argv[1] if len(sys.argv) > 1 else "busy"
SERVICE = 0x8000
if MODE not in ("busy", "missing", "multiple"):
    raise ValueError("unknown network-service test mode")

sst.setProgramOption("timebase", "1ps")

ports = 2 if MODE == "busy" else 1
router = sst.Component("router", "merlin.hr_router")
router.addParams({
    "id": 0, "num_ports": ports, "num_vns": 1,
    "link_bw": "8GB/s", "flit_size": "8B", "xbar_bw": "8GB/s",
    "input_latency": "0ns", "output_latency": "0ns",
    "input_buf_size": "8B", "output_buf_size": "8B",
})
router.setSubComponent("topology", "merlin.singlerouter")
if MODE == "busy":
    router.setSubComponent("network_service", "merlin.network_service_probe_processor")

service_ids = [SERVICE, SERVICE + 1] if MODE == "multiple" else [SERVICE]
for endpoint_id in range(ports):
    endpoint = sst.Component("endpoint%d" % endpoint_id, "merlin.network_service_probe_endpoint")
    endpoint.addParam("id", endpoint_id)
    network_if = endpoint.setSubComponent("networkIF", "merlin.linkcontrol")
    network_if.addParams({
        "link_bw": "8GB/s", "input_buf_size": "8B", "output_buf_size": "8B",
        "network_service_ids": service_ids,
    })
    link = sst.Link("endpoint%d_link" % endpoint_id)
    link.connect((network_if, "rtr_port", "1ns"), (router, "port%d" % endpoint_id, "1ns"))
    link.setNoCut()
