# Runs tests/hooks.cc on one node. --mode picks the hooks it calls.

import argparse
import sst
from sst.merlin.base import *
from sst.merlin.endpoint import *
from sst.merlin.interface import *
from sst.merlin.topology import *
from sst.hg import *

parser = argparse.ArgumentParser()
parser.add_argument("--mode", default="free")
args = parser.parse_args()

platdef = PlatformDefinition("platform_hg_hooks")
PlatformDefinition.registerPlatformDefinition(platdef)
platdef.addParamSet("node", {"name" : "hg.NodeCL"})
platdef.addParamSet("nic", {"mtu" : "4096 B"})
platdef.addParamSet("operating_system", {"name" : "hg.OperatingSystemCL"})
platdef.addParamSet("topology", {"link_latency" : "20ns", "num_ports" : "1"})
platdef.addParamSet("network_interface", {
    "link_bw" : "11.25 GB/s",
    "input_buf_size" : "32kB",
    "output_buf_size" : "32kB",
})
platdef.addClassType("network_interface", "sst.merlin.interface.ReorderLinkControl")
platdef.addParamSet("router", {
    "link_bw" : "11.25 GB/s",
    "flit_size" : "8B",
    "xbar_bw" : "50GB/s",
    "input_latency" : "20ns",
    "output_latency" : "20ns",
    "input_buf_size" : "32kB",
    "output_buf_size" : "32kB",
    "num_vns" : 1,
    "xbar_arb" : "merlin.xbar_arb_lru",
})
platdef.addClassType("router", "sst.merlin.base.hr_router")

PlatformDefinition.setCurrentPlatform("platform_hg_hooks")
platform = PlatformDefinition.getCurrentPlatform()
platform.addParamSet("operating_system", {
    "app1.name" : "hooks",
    "app1.exe_library_name" : "hooks",
    "app1.argv" : args.mode,
})

topo = topoSingle()
topo.num_ports = 1
ep = HgJob(0, 1)
system = System()
system.setTopology(topo)
system.allocateNodes(ep, "linear")
system.build()
