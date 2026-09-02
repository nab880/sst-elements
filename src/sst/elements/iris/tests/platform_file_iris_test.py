# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S.
# Government retains certain rights in this software.

from sst.merlin.base import PlatformDefinition

platform = PlatformDefinition("platform_iris_test")
PlatformDefinition.registerPlatformDefinition(platform)

platform.addParamSet("node", {
    "verbose": "0",
    "name": "hg.NodeCL",
    "negligible_compute_bytes": "64B",
    "parallelism": "1.0",
    "frequency": "2.1GHz",
    "flow_mtu": "512",
    "channel_bandwidth": "11.2 GB/s",
    "num_channels": "4",
})
platform.addParamSet("nic", {"verbose": "0", "mtu": "4096 B"})
platform.addParamSet("operating_system", {
    "verbose": "0",
    "name": "hg.OperatingSystemCL",
    "ncores": "24",
    "nsockets": "4",
    "app1.post_rdma_delay": "1.5us",
    "app1.post_header_delay": "0.5us",
    "app1.poll_delay": "0us",
    "app1.rdma_pin_latency": "5.43us",
    "app1.rdma_page_delay": "50.50ns",
    "app1.rdma_page_size": "4096",
    "app1.max_vshort_msg_size": "4096 B",
    "app1.max_eager_msg_size": "32768 B",
    "app1.use_put_window": "false",
    "app1.compute_library_access_width": "64",
    "app1.compute_library_loop_overhead": "1.0",
})
platform.addParamSet("topology", {"link_latency": "20ns", "num_ports": "32"})
platform.addParamSet("network_interface", {
    "link_bw": "11.25 GB/s",
    "input_buf_size": "32kB",
    "output_buf_size": "32kB",
})
platform.addClassType("network_interface", "sst.merlin.interface.ReorderLinkControl")
platform.addParamSet("router", {
    "link_bw": "11.25 GB/s",
    "flit_size": "8B",
    "xbar_bw": "50GB/s",
    "input_latency": "20ns",
    "output_latency": "20ns",
    "input_buf_size": "32kB",
    "output_buf_size": "32kB",
    "num_vns": 1,
    "xbar_arb": "merlin.xbar_arb_lru",
})
platform.addClassType("router", "sst.merlin.base.hr_router")
