#!/usr/bin/env python3
# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S.
# Government retains certain rights in this software.

import argparse

from sst.merlin.base import PlatformDefinition, System
from sst.merlin.topology import topoSingle
from sst.hg import HgJob

parser = argparse.ArgumentParser()
parser.add_argument("--nodes", type=int, default=4)
parser.add_argument("--eager-cutoff", type=int, default=32768)
parser.add_argument("--use-put-protocol", action="store_true")
args = parser.parse_args()

PlatformDefinition.loadPlatformFile("platform_file_iris_test")
PlatformDefinition.setCurrentPlatform("platform_iris_test")
platform = PlatformDefinition.getCurrentPlatform()
platform.addParamSet("operating_system", {
    "app1.name": "sumicollectives",
    "app1.exe_library_name": "sumicollectives",
    "app1.dependencies": ["sumi"],
    "app1.libraries": ["computelibrary:ComputeLibrary",
                       "sumicollectives:sumi"],
    "app1.collective.reduce_scatter": "halving",
    "app1.eager_cutoff": args.eager_cutoff,
    "app1.use_put_protocol": args.use_put_protocol,
})

topology = topoSingle()
topology.link_latency = "20ns"
topology.num_ports = max(32, args.nodes)

job = HgJob(0, args.nodes)
system = System()
system.setTopology(topology)
system.allocateNodes(job, "random", 42)
system.build()
