# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.

import sst


sst.setProgramOption("timebase", "1ps")
sst.setStatisticOutput("sst.statOutputConsole")

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
    "input_buf_size": "32B",
    "output_buf_size": "32B",
    "xbar_arb": "merlin.xbar_arb_rr",
})
router.setSubComponent("topology", "merlin.singlerouter")

injector = sst.Component("injector", "firefly.empty_request_regression_test")
injector.addParams({"destination": 1, "vn": 0, "request_bits": 64})
injector_if = injector.setSubComponent("networkIF", "merlin.linkcontrol")
injector_if.addParams({
    "link_bw": "8GB/s",
    "input_buf_size": "32B",
    "output_buf_size": "32B",
})

nic = sst.Component("nic", "firefly.nic")
nic.addParams({
    "nid": 1,
    "num_vNics": 1,
    "numVNs": 1,
    "packetSize": "32B",
    "nic2host_lat": "1ns",
})
nic.enableStatistics([
    "rcvdPkts",
    "collectiveEnqueued",
    "collectiveSchedulerSends",
    "collectiveSendRetries",
    "collectiveResultsCompleted",
], {"type": "sst.AccumulatorStatistic"})
nic_if = nic.setSubComponent("rtrLink", "merlin.linkcontrol")
nic_if.addParams({
    "link_bw": "8GB/s",
    "input_buf_size": "32B",
    "output_buf_size": "32B",
})

host_link = sst.Link("host_link")
host_link.connect((injector, "nic", "1ns"), (nic, "core0", "1ns"))
host_link.setNoCut()

injector_link = sst.Link("injector_link")
injector_link.connect((injector_if, "rtr_port", "1ns"), (router, "port0", "1ns"))
injector_link.setNoCut()

nic_link = sst.Link("nic_link")
nic_link.connect((nic_if, "rtr_port", "1ns"), (router, "port1", "1ns"))
nic_link.setNoCut()
