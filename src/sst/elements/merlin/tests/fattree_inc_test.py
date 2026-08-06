#!/usr/bin/env python
#
# Copyright 2009-2025 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S.
# Government retains certain rights in this software.
#
# Copyright (c) 2009-2025, NTESS
# All rights reserved.
#
# This file is part of the SST software package. For license
# information, see the LICENSE file in the top level directory of the
# distribution.

import sst
from sst.merlin.base import *
from sst.merlin.endpoint import *
from sst.merlin.interface import *
from sst.merlin.topology import *

if __name__ == "__main__":
    ### Setup the topology
    # Shape "4,2:4" -> 16 hosts
    # Level 0: 4 edge routers (radix 6: 4 down + 2 up)
    # Level 1: 2 top routers (radix 4: 4 down)
    topo = topoFatTree()
    topo.shape = "4,2:4"
    topo.link_latency = "20ns"

    # Set up the routers
    router = hr_router()
    router.link_bw = "4GB/s"
    router.flit_size = "8B"
    router.xbar_bw = "4GB/s"
    router.input_latency = "20ns"
    router.output_latency = "20ns"
    router.input_buf_size = "4kB"
    router.output_buf_size = "4kB"
    router.num_vns = 1
    router.xbar_arb = "merlin.xbar_arb_lru"

    topo.router = router

    ### Set up the INC endpoint
    networkif = LinkControl()
    networkif.link_bw = "4GB/s"
    networkif.input_buf_size = "1kB"
    networkif.output_buf_size = "1kB"

    ep = INCJob(0, topo.getNumNodes())
    ep.network_interface = networkif
    ep.message_size = "64B"
    ep.num_messages = 1
    ep.shape = "4,2:4"

    system = System()
    system.setTopology(topo)
    system.allocateNodes(ep, "linear")
    system.build()

    ### Wire accelerator subcomponents onto every router port (post-build).
    # The collective_accel subcomponents form a unidirectional ring per router,
    # connected via lport/rport links.

    def wireAccelerators(rtr_name, rtr_id, radix):
        rtr = sst.findComponentByName(rtr_name)
        if rtr is None:
            return

        accels = []
        for p in range(radix):
            accels.append(
                rtr.setSubComponent("accelerator%d" % p, "merlin.collective_accel")
            )

        # Wire the ring: each accel's rport connects to the next accel's lport
        for p in range(radix):
            link = sst.Link("rtr%d_accellink%d" % (rtr_id, p))
            link.connect(
                (accels[p], "rport", "10ns"), (accels[(p + 1) % radix], "lport", "10ns")
            )

    # Fat-tree shape "4,2:4":
    #   Level 0: 4 edge routers (ids 0-3), radix = 4+2 = 6
    #   Level 1: 2 top routers  (ids 4-5), radix = 4
    downs = [4, 4]
    ups = [2]
    num_hosts = 16
    num_levels = len(downs)

    routers_per_level = [0] * num_levels
    routers_per_level[0] = num_hosts // downs[0]
    for i in range(1, num_levels):
        routers_per_level[i] = routers_per_level[i - 1] * ups[i - 1] // downs[i]

    start_ids = [0] * num_levels
    for i in range(1, num_levels):
        start_ids[i] = start_ids[i - 1] + routers_per_level[i - 1]

    groups_per_level = [1] * num_levels
    groups_per_level[0] = num_hosts // downs[0]
    for i in range(1, num_levels - 1):
        groups_per_level[i] = groups_per_level[i - 1] // downs[i]

    for level in range(num_levels):
        rtrs_in_level = routers_per_level[level]
        groups = groups_per_level[level]
        rtrs_per_group = rtrs_in_level // groups

        if level < len(ups):
            radix = downs[level] + ups[level]
        else:
            radix = downs[level]

        for g in range(groups):
            for r in range(rtrs_per_group):
                rtr_id = start_ids[level] + g * rtrs_per_group + r
                rtr_name = "rtr_l%d_g%d_r%d" % (level, g, r)
                wireAccelerators(rtr_name, rtr_id, radix)
