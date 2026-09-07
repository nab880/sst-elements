# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S.
# Government retains certain rights in this software.

import sst


sst.setProgramOption("timebase", "1ps")

router = sst.Component("router", "merlin.hr_router")
router.addParams({
    "id": 0,
    "num_ports": 2,
    "num_vns": 1,
    "link_bw": "8GB/s",
    "flit_size": "8B",
    "xbar_bw": "8GB/s",
    "input_latency": "1ns",
    "output_latency": "1ns",
    "input_buf_size": "64B",
    "output_buf_size": "64B",
    "xbar_arb": "merlin.xbar_arb_rr",
})
router.setSubComponent("topology", "merlin.singlerouter")

for node_id in range(2):
    node = sst.Component("node%d" % node_id, "hg.Node")
    node.addParams({
        "logicalID": node_id,
        "nranks": 2,
        "npernode": 1,
        "num_vns": 4,
        "ordinary_vn": 0,
        "manager_vn": 0,
        "reduce_vn": 2,
        "result_vn": 1,
    })

    os = node.setSubComponent("os_slot", "hg.OperatingSystem")
    os.addParams({
        "app1.name": "manager_vn_smoke",
        "app1.exe_library_name": "manager_vn_smoke",
    })
    node.setSubComponent("nic_slot", "hg.nic")
    network_if = node.setSubComponent(
        "link_control_slot", "merlin.linkcontrol"
    )
    network_if.addParams({
        "link_bw": "8GB/s",
        "input_buf_size": "64B",
        "output_buf_size": "64B",
        # Manager and ordinary native traffic intentionally share VN 0.
        "vn_remap": [0, -1, -1, -1],
    })

    link = sst.Link("endpoint%d_link" % node_id)
    link.connect(
        (network_if, "rtr_port", "1ns"),
        (router, "port%d" % node_id, "1ns"),
    )
    link.setNoCut()
