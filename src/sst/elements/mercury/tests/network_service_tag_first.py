# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.

import sst


SERVICE_ID = 0x8000
sst.setProgramOption("timebase", "1ps")

router = sst.Component("router", "merlin.hr_router")
router.addParams({
    "id": 0, "num_ports": 2, "num_vns": 4,
    "link_bw": "8GB/s", "flit_size": "8B", "xbar_bw": "8GB/s",
    "input_latency": "0ns", "output_latency": "0ns",
    "input_buf_size": "8B", "output_buf_size": "8B",
    "xbar_arb": "merlin.xbar_arb_rr", "network_service_output_queue_depth": 2,
})
router.setSubComponent("topology", "merlin.singlerouter")
router.setSubComponent("network_service", "merlin.network_service_probe_processor")

source = sst.Component("source", "merlin.network_service_probe_endpoint")
source.addParam("id", 1)
source_if = source.setSubComponent("networkIF", "merlin.linkcontrol")

mercury = sst.Component("mercury", "hg.Node")
mercury.addParams({
    "logicalID": 1, "nranks": 1, "npernode": 1,
    "num_vns": 4, "ordinary_vn": 3, "manager_vn": 0,
    "reduce_vn": 2, "result_vn": 1,
})
mercury_os = mercury.setSubComponent("os_slot", "hg.OperatingSystem")
mercury_os.addParams({"app1.name": "ostest", "app1.exe_library_name": "ostest"})
mercury.setSubComponent("nic_slot", "hg.nic")
mercury_if = mercury.setSubComponent("link_control_slot", "merlin.linkcontrol")

for endpoint_id, network_if in enumerate((mercury_if, source_if)):
    network_if.addParams({
        "link_bw": "8GB/s", "input_buf_size": "8B", "output_buf_size": "8B",
        "network_service_ids": [SERVICE_ID],
    })
    link = sst.Link("endpoint%d_link" % endpoint_id)
    link.connect((network_if, "rtr_port", "1ns"),
                 (router, "port%d" % endpoint_id, "1ns"))
    link.setNoCut()
