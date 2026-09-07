# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.

"""Reject remapped service VNs while preserving identity and ordinary traffic."""

import sst


sst.setProgramOption("timebase", "1ps")

for name, mapping, negotiated in (
    ("permutation", [0, 2, 1], False),
    ("alias", [0, 0, 2], False),
    ("negotiated", [0, 2, 1], True),
    ("no_identity", [1, 2, 0], False),
):
    router = sst.Component(name + "_router", "merlin.hr_router")
    router.addParams({
        "id": 0,
        "num_ports": 1,
        "num_vns": 3,
        "link_bw": "8GB/s",
        "flit_size": "8B",
        "xbar_bw": "8GB/s",
        "input_latency": "0ns",
        "output_latency": "0ns",
        "input_buf_size": "8B",
        "output_buf_size": "8B",
        "xbar_arb": "merlin.xbar_arb_rr",
    })
    if negotiated:
        # This permutation is its own inverse. Router negotiation sends the
        # effective injection mapping to LinkControl without its vn_remap param.
        router.addParams({"vn_remap_shm": name + "_map", "vn_remap": mapping})
    router.setSubComponent("topology", "merlin.singlerouter")
    processor = router.setSubComponent("network_service", "merlin.network_service_pass")
    processor.addParam("service_id", 0x8000)

    endpoint = sst.Component(name, "merlin.network_service_vn_remap_endpoint")
    endpoint.addParam("expected_map", mapping)
    network_if = endpoint.setSubComponent("networkIF", "merlin.linkcontrol")
    network_if.addParams({
        "link_bw": "8GB/s",
        "input_buf_size": "8B",
        "output_buf_size": "8B",
        "network_service_id": 0x8000,
    })
    if not negotiated:
        network_if.addParam("vn_remap", mapping)

    link = sst.Link(name + "_link")
    link.connect((network_if, "rtr_port", "1ns"), (router, "port0", "1ns"))
    link.setNoCut()
