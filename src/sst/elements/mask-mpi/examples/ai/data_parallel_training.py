#!/usr/bin/env python3
# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights
# in this software.

"""Run synchronous SGD through Mask-MPI on Mercury's collective endpoint."""

import sys

import sst
from sst.hg import *
from sst.merlin.base import *
from sst.merlin.collective import *
from sst.merlin.topology import *


SCENARIO = sys.argv[1] if len(sys.argv) > 1 else "scalar"
PATH = sys.argv[2] if len(sys.argv) > 2 else "offload"
EPOCHS = int(sys.argv[3]) if len(sys.argv) > 3 else 8
COMPUTE_NS = int(sys.argv[4]) if len(sys.argv) > 4 else 500

if SCENARIO not in ("scalar", "hybrid"):
    raise ValueError("scenario must be scalar or hybrid")
if PATH not in ("software", "offload"):
    raise ValueError("path must be software or offload")
if EPOCHS <= 0 or COMPUTE_NS < 0:
    raise ValueError("epochs must be positive and compute time must be nonnegative")
if len(sys.argv) > 5:
    raise ValueError("expected: scenario path epochs compute_ns")

RANKS = 8
SERVICE_ID = 1

sst.setProgramOption("timebase", "1ps")
sst.setStatisticOutput("sst.statOutputConsole")

# The binary fat tree below; the collective tree is derived from it after
# the job is allocated.
topology = topoFatTree()
topology.shape = "2,1:2,1:2"
topology.routing_alg = "deterministic"
topology.link_latency = "20ns"
topology.host_link_latency = "20ns"

platform = PlatformDefinition("mercury-ai-training")
PlatformDefinition.registerPlatformDefinition(platform)
platform.addClassType(
    "network_interface", "sst.merlin.interface.ReorderLinkControl"
)
platform.addParamSet(
    "node",
    {
        "name": "hg.NodeCL",
        "verbose": 0,
        "negligible_compute_bytes": "64B",
        "parallelism": 1.0,
        "frequency": "2.1GHz",
        "flow_mtu": 512,
        "channel_bandwidth": "11.2GB/s",
        "num_channels": 4,
        # Native and manager traffic share VN 0; the collective plan below
        # claims VNs 1 and 2.
        "num_vns": 3,
        "ordinary_vn": 0,
        "manager_vn": 0,
    },
)
platform.addParamSet(
    "nic",
    {
        "verbose": 0,
        "mtu": "4096B",
    },
)
platform.addParamSet(
    "operating_system",
    {
        "name": "hg.OperatingSystemCL",
        "verbose": 0,
        "ncores": 1,
        "nsockets": 1,
        "app1.name": "mercury_ai_training",
        "app1.exe_library_name": "mercury_ai_training",
        "app1.argv": f"{SCENARIO} {EPOCHS} {COMPUTE_NS}",
        "app1.dependencies": ["sumi"],
        "app1.libraries": [
            "computelibrary:ComputeLibrary",
            "mask_mpi:MpiApi",
        ],
        "app1.enable_collective_offload": PATH == "offload",
        "app1.post_rdma_delay": "1.5us",
        "app1.post_header_delay": "0.5us",
        "app1.poll_delay": "0us",
        "app1.rdma_pin_latency": "5.43us",
        "app1.rdma_page_delay": "50.50ns",
        "app1.rdma_page_size": 4096,
        "app1.max_vshort_msg_size": "4096B",
        "app1.max_eager_msg_size": "32768B",
        "app1.use_put_window": False,
        "app1.compute_library_access_width": 64,
        "app1.compute_library_loop_overhead": 1.0,
    },
)
platform.addParamSet(
    "network_interface",
    {
        "link_bw": "8GB/s",
        "input_buf_size": "4kB",
        "output_buf_size": "4kB",
        # Only the collective job requires service-capable router ports.
        "network_service_id": 0,
    },
)
PlatformDefinition.setCurrentPlatform("mercury-ai-training")

job = HgJob(0, RANKS)
job.network_interface.network_service_id = SERVICE_ID
system = System()
system.setTopology(topology)
system.allocateNodes(job, "linear")

plan = StaticCollectivePlan.from_fattree(topology, logical_ids=job, reduce_vn=1, result_vn=2)
job.useCollectivePlan(plan)
topology.router = StaticCollectiveRouter(plan, enable_statistics=True)
topology.router.link_bw = "8GB/s"
topology.router.flit_size = "8B"
topology.router.xbar_bw = "8GB/s"
topology.router.input_latency = "20ns"
topology.router.output_latency = "20ns"
topology.router.input_buf_size = "4kB"
topology.router.output_buf_size = "4kB"
topology.router.num_vns = 3
topology.router.xbar_arb = "merlin.xbar_arb_lru"
system.build()

print(
    f"Mercury AI training demo scenario={SCENARIO} path={PATH} "
    f"ranks={RANKS} epochs={EPOCHS} compute_ns={COMPUTE_NS}"
)
