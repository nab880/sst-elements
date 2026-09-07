# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.

import sys

import sst
from sst.merlin.collective import StaticCollectivePlan


MODE = sys.argv[1] if len(sys.argv) > 1 else ""
SERVICE_ENABLED = MODE not in ("", "disabled")
ARBITER = sys.argv[2] if len(sys.argv) > 2 else None
WIDE_FLIT = MODE == "wide-flit"
BAD_CAPACITY = MODE == "bad-capacity"
BAD_DOWNSTREAM_CAPACITY = MODE == "bad-downstream-capacity"
DISCONNECTED = MODE == "disconnected"
SHAPE = "2,1:2"

sst.setProgramOption("timebase", "1ps")

# The test NIC keeps its ordinary traffic on VN 0; the processor owns VNs 1
# and 2 on every router in the tree.
collective_plan = StaticCollectivePlan(
    root_router=2,
    router_links=((0, 2, 2, 0), (1, 2, 2, 1)),
    endpoint_links=tuple((nid, nid, nid // 2, nid % 2) for nid in range(4)),
    reduce_vn=1,
    result_vn=2,
)


def make_router(router_id, radix):
    router = sst.Component("router%d" % router_id, "merlin.hr_router")
    router.addParams({
        "id": router_id,
        "num_ports": radix,
        "num_vns": 3,
        "link_bw": "8GB/s",
        "flit_size": "16B" if WIDE_FLIT else "8B",
        "xbar_bw": "8GB/s",
        "input_latency": "0ns",
        "output_latency": "0ns",
        "input_buf_size": "8B" if BAD_DOWNSTREAM_CAPACITY and router_id == 2 else "256B",
        "output_buf_size": "8B" if BAD_CAPACITY else "256B",
        "network_service_output_queue_depth": 1,
    })
    if ARBITER:
        router.addParam("xbar_arb", "merlin.xbar_arb_" + ARBITER)
    router.setSubComponent("topology", "merlin.fattree").addParams({
        "shape": SHAPE,
        "routing_alg": "deterministic",
    })
    if SERVICE_ENABLED:
        processor = router.setSubComponent(
            "network_service", "merlin.collective_static_processor"
        )
        processor.addParams(collective_plan.processor_params(router_id))
    return router


routers = [
    make_router(0, 3),
    make_router(1, 3),
    make_router(2, 2),
]

network_interfaces = []
for endpoint_id in range(4):
    endpoint = sst.Component("endpoint%d" % endpoint_id, "merlin.test_nic")
    endpoint.addParams({
        "id": endpoint_id,
        "num_peers": 4,
        "num_vns": 3,
        "num_messages": 64,
        "message_size": "512b",
    })
    network_if = endpoint.setSubComponent("networkIF", "merlin.linkcontrol")
    # All three VNs stay requested so every VC advertises credits, whether or
    # not the service is installed.
    network_if.addParams({
        "link_bw": "8GB/s",
        "input_buf_size": "256B",
        "output_buf_size": "256B",
    })
    network_interfaces.append(network_if)

for endpoint_id, (router_id, router_port) in enumerate(((0, 0), (0, 1), (1, 0), (1, 1))):
    link = sst.Link("endpoint%d_link" % endpoint_id)
    link.connect(
        (network_interfaces[endpoint_id], "rtr_port", "1ns"),
        (routers[router_id], "port%d" % router_port, "1ns"),
    )
    link.setNoCut()

for leaf_id, root_port in ((0, 0), (1, 1)):
    if DISCONNECTED and leaf_id == 0:
        continue
    link = sst.Link("leaf%d_root_link" % leaf_id)
    link.connect(
        (routers[leaf_id], "port2", "1ns"),
        (routers[2], "port%d" % root_port, "1ns"),
    )
    link.setNoCut()
