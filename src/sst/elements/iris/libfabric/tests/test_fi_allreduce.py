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
#   SUMI_ALLREDUCE_ALG=ring select the registry algorithm; the result must
#                           stay correct while the reported time changes.

import os
import sst
from sst.merlin.base import *
from sst.merlin.endpoint import *
from sst.merlin.interface import *
from sst.merlin.topology import *
from sst.hg import *

if __name__ == "__main__":

    PlatformDefinition.loadPlatformFile("platform_file_mask_mpi_test")
    PlatformDefinition.setCurrentPlatform("platform_mask_mpi_test")
    platform = PlatformDefinition.getCurrentPlatform()

    _alg = os.environ.get("SUMI_ALLREDUCE_ALG", "")
    os_params = {
        "verbose" : "0",
        "app1.name" : "fi_allreduce_test",
        "app1.exe_library_name" : "fi_allreduce_test",
        "app1.dependencies" : ["sumi", "fabric", ],
        "app1.libraries" : ["computelibrary:ComputeLibrary",
                            "macro:libfabric", ],
    }
    if _alg:
        os_params["app1.allreduce_alg"] = _alg
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
