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

import sst
from sst.ember import *
from sst.merlin.base import *
from sst.merlin.interface import *
from sst.merlin.topology import *


sst.setProgramOption("timebase", "1ps")
sst.setStatisticOutput("sst.statOutputConsole")

PlatformDefinition.setCurrentPlatform("firefly-defaults")

topology = topoFatTree()
topology.shape = "2,1:2"
topology.routing_alg = "deterministic"
topology.router = hr_router()
topology.router.link_bw = "8GB/s"
topology.router.flit_size = "8B"
topology.router.xbar_bw = "8GB/s"
topology.router.input_latency = "0ns"
topology.router.output_latency = "0ns"
topology.router.input_buf_size = "256B"
topology.router.output_buf_size = "256B"
topology.router.num_vns = 1
topology.router.xbar_arb = "merlin.xbar_arb_lru"
topology.link_latency = "1ns"
topology.host_link_latency = "1ns"

networkif = ReorderLinkControl()
networkif.link_bw = "8GB/s"
networkif.input_buf_size = "256B"
networkif.output_buf_size = "256B"

job = EmberMPIJob(0, 4)
job.network_interface = networkif
job.addMotif("Init")
job.addMotif("Allreduce iterations=2 compute=0 count=4 verify=true")
job.addMotif("Fini")
job.nic.numVNs = 1

system = System()
system.setTopology(topology)
system.allocateNodes(job, "linear")
system.build()

for statistic in ("sentPkts", "rcvdPkts"):
    sst.enableStatisticForComponentType(
        "firefly.nic", statistic, {"type": "sst.AccumulatorStatistic"}
    )
