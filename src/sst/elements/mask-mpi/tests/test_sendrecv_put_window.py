#!/usr/bin/env python3
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

import argparse
import sst
from sst.merlin.base import *
from sst.merlin.endpoint import *
from sst.merlin.interface import *
from sst.merlin.topology import *
from sst.hg import *

parser = argparse.ArgumentParser()
parser.add_argument("--nodes", type=int, default=2)
parser.add_argument("--cores", type=int, default=1)
parser.add_argument("--vector", action="store_true",
                    help="send a strided MPI_Type_vector, which DirectPut must pack")
args = parser.parse_args()

if __name__ == "__main__":

    PlatformDefinition.loadPlatformFile("platform_file_mask_mpi_test")
    PlatformDefinition.setCurrentPlatform("platform_mask_mpi_test")
    platform = PlatformDefinition.getCurrentPlatform()

    params = {
        "app1.name" : "sendrecv",
        "app1.exe_library_name" : "sendrecv",
        # Route every message through the direct-put protocol; the sendrecv
        # skeleton validates the received values, so this checks that
        # DirectPut delivers payload and not only timing.
        "app1.use_put_window" : "true",
        "app1.dependencies" : ["sumi", ],
        "app1.libraries" : ["computelibrary:ComputeLibrary",
                            "mask_mpi:MpiApi",],
    }
    if args.vector:
        params["app1.argv"] = "vector"
    platform.addParamSet("operating_system", params)

    topo = topoSingle()
    topo.link_latency = "20ns"
    topo.num_ports = 32

    # Two nodes go over the network; one node with two cores uses the
    # intranode path, which stages the payload in NetworkMessage.
    ep = HgJob(0, args.nodes, numCores=args.cores)

    system = System()
    system.setTopology(topo)
    system.allocateNodes(ep,"linear")

    system.build()
