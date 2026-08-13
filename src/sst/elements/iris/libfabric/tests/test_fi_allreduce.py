#!/usr/bin/env python
#
# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S.
# Government retains certain rights in this software.
#
# Copyright (c) 2009-2026, NTESS
# All rights reserved.
#
# This file is part of the SST software package. For license
# information, see the LICENSE file in the top level directory of the
# distribution.
#
# Driver for the raw-libfabric FI_COLLECTIVE end-to-end test
# (fi_allreduce_test.cc). Runs with just `sst` -- no hgcc, no MVAPICH2.
#
#   NRANKS=<n>              change the rank count (default 8)
#   SUMI_ALLREDUCE_ALG=ring   select through the environment
#   SUMI_ALLREDUCE_PARAM=ring select through app1.collective.allreduce

import os
import sst
from sst.merlin.base import *
from sst.merlin.endpoint import *
from sst.merlin.interface import *
from sst.merlin.topology import *
from sst.hg import *

platdef = PlatformDefinition("platform_iris_fi_collective_test")
PlatformDefinition.registerPlatformDefinition(platdef)
platdef.addParamSet("node", {
    "verbose": "0", "name": "hg.NodeCL", "negligible_compute_bytes": "64B",
    "parallelism": "1.0", "frequency": "2.1GHz", "flow_mtu": "512",
    "channel_bandwidth": "11.2 GB/s", "num_channels": "4",
})
platdef.addParamSet("nic", {"verbose": "0", "mtu": "4096 B"})
platdef.addParamSet("operating_system", {
    "verbose": "0", "name": "hg.OperatingSystemCL", "ncores": "24",
    "nsockets": "4", "app1.post_rdma_delay": "1.5us",
    "app1.post_header_delay": "0.5us", "app1.poll_delay": "0us",
    "app1.rdma_pin_latency": "5.43us", "app1.rdma_page_delay": "50.50ns",
    "app1.rdma_page_size": "4096", "app1.max_vshort_msg_size": "4096 B",
    "app1.max_eager_msg_size": "32768 B", "app1.use_put_window": "false",
    "app1.compute_library_access_width": "64",
    "app1.compute_library_loop_overhead": "1.0",
})
platdef.addParamSet("topology", {"link_latency": "20ns", "num_ports": "32"})
platdef.addParamSet("network_interface", {
    "link_bw": "11.25 GB/s", "input_buf_size": "32kB",
    "output_buf_size": "32kB",
})
platdef.addParamSet("router", {
    "link_bw": "11.25 GB/s", "flit_size": "8B", "xbar_bw": "50GB/s",
    "input_latency": "20ns", "output_latency": "20ns",
    "input_buf_size": "32kB", "output_buf_size": "32kB", "num_vns": 1,
    "xbar_arb": "merlin.xbar_arb_lru",
})
platdef.addClassType("network_interface", "sst.merlin.interface.ReorderLinkControl")
platdef.addClassType("router", "sst.merlin.base.hr_router")

if __name__ == "__main__":

    PlatformDefinition.setCurrentPlatform("platform_iris_fi_collective_test")
    platform = PlatformDefinition.getCurrentPlatform()

    _param_alg = os.environ.get("SUMI_ALLREDUCE_PARAM", "")
    os_params = {
        "verbose" : "0",
        "app1.name" : "fi_allreduce_test",
        "app1.exe_library_name" : "fi_allreduce_test",
        "app1.dependencies" : ["sumi", "fabric", ],
        "app1.libraries" : ["computelibrary:ComputeLibrary",
                            "macro:libfabric", ],
    }
    if _param_alg:
        os_params["app1.collective.allreduce"] = _param_alg
    platform.addParamSet("operating_system", os_params)

    _nranks = int(os.environ.get("NRANKS", "8"))
    topo = topoSingle()
    topo.link_latency = "20ns"
    topo.num_ports = max(32, _nranks)

    ep = HgJob(0, _nranks)

    system = System()
    system.setTopology(topo)
    system.allocateNodes(ep, "random", 42)

    system.build()
