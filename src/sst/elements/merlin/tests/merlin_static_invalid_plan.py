# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.

import sys

from sst.merlin.collective import StaticCollectivePlan
from sst.merlin.topology import topoFatTree


MODE = sys.argv[1] if len(sys.argv) > 1 else "overflow"

if MODE == "overflow":
    StaticCollectivePlan(1 << 31, (), ((0, 0, 0, 0),))
elif MODE == "wrong-root":
    topology = topoFatTree()
    topology.shape = "2,1:2"
    StaticCollectivePlan.from_fattree(topology, root_router=99)
else:
    raise ValueError("mode must be overflow or wrong-root")

raise RuntimeError("invalid static collective plan was accepted")
