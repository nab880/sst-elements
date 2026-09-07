# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.

from sst.merlin.collective import StaticCollectivePlan, StaticCollectiveRouter
from sst.merlin.topology import topoFatTree
from unittest.mock import patch


ROUTER_LINKS = ((0, 2, 2, 0), (1, 2, 2, 1))
ENDPOINT_LINKS = tuple((nid, nid, nid // 2, nid % 2) for nid in range(4))


def require_rejected(message, root_router=2, router_links=ROUTER_LINKS,
                     endpoint_links=ENDPOINT_LINKS, **kwargs):
    try:
        StaticCollectivePlan(root_router, router_links, endpoint_links, **kwargs)
    except ValueError:
        return
    raise RuntimeError("StaticCollectivePlan accepted " + message)


def rejects(action, diagnostic):
    try:
        action()
    except ValueError as error:
        assert diagnostic in str(error), str(error)
        return
    raise RuntimeError("StaticCollectivePlan accepted " + diagnostic)


class Job:
    def __init__(self, mapping, size=None):
        self._nid_map = mapping
        self.size = len(mapping) if size is None else size

    def getSize(self):
        return self.size


plan = StaticCollectivePlan(2, ROUTER_LINKS, ENDPOINT_LINKS)
assert plan.router_ids == (0, 1, 2)
assert plan.endpoint_ids == (0, 1, 2, 3)
assert plan.endpoint(3)["logical_participant_id"] == 3
assert plan.processor_params(0)["parent_port"] == 2
assert plan.processor_params(2)["child_nids"] == [0, 2]
assert StaticCollectiveRouter(plan).clone()._collective_plan is plan

# Deriving the plan from the fat tree reproduces the hand-written one.
topology = topoFatTree()
topology.shape = "2,1:2"
derived = StaticCollectivePlan.from_fattree(topology)
assert derived.router_ids == plan.router_ids and derived.endpoint_ids == plan.endpoint_ids
assert derived.root_nid == plan.root_nid and derived.root_logical_nid == plan.root_logical_nid
for router in plan.router_ids:
    assert derived.processor_params(router) == plan.processor_params(router), router
for nid in plan.endpoint_ids:
    assert dict(derived.endpoint(nid)) == dict(plan.endpoint(nid)), nid
mapped = StaticCollectivePlan.from_fattree(topology, logical_ids={0: 2, 1: 1, 2: 3, 3: 0})
assert mapped.endpoint(0)["logical_participant_id"] == 2
assert mapped.root_logical_nid == 2

# A sparse allocation selects its exact hosts without renumbering host ports.
job = Job({1: 1, 3: 0})
sparse = StaticCollectivePlan.from_fattree(topology, logical_ids=job)
assert sparse.endpoint_ids == (1, 3)
assert sparse.processor_params(0)["local_ports"] == [1]
assert sparse.processor_params(1)["local_ports"] == [1]
assert sparse.processor_params(2)["child_nids"] == [1, 3]
assert sparse.root_nid == 1 and sparse.root_logical_nid == 1
sparse.validate_job(job)
for physical, logical in job._nid_map.items():
    sparse.validate_participant(physical, logical)
rejects(lambda: plan.validate_job(job), "physical membership")
rejects(lambda: sparse.validate_job(Job({1: 0, 3: 1})), "logical membership")
rejects(lambda: sparse.validate_job(Job({1: 0, 3: 0})), "every logical rank")
rejects(lambda: sparse.validate_job(Job({1: 0}, size=2)), "every logical rank")
job._nid_map = {0: 1, 3: 0}
rejects(lambda: sparse.validate_job(job), "physical membership")
rejects(lambda: sparse.validate_participant(0, 1), "bound job allocation")
rejects(lambda: sparse.validate_participant(1, 0), "bound job allocation")
rejects(lambda: StaticCollectivePlan.from_fattree(topology, logical_ids={}), "at least one")
rejects(lambda: StaticCollectivePlan.from_fattree(topology, logical_ids={4: 0}), "outside root_router")
rejects(lambda: StaticCollectivePlan.from_fattree(topology, root_router=0, logical_ids={3: 0}),
        "outside root_router")
unallocated = Job({}, size=2)
unallocated._nid_map = None
rejects(lambda: sparse.validate_job(unallocated), "not been allocated")
linear = Job(list(range(4)))
plan.validate_job(linear)
assert StaticCollectivePlan.from_fattree(topology, logical_ids=linear).endpoint_ids == plan.endpoint_ids

# Endpoint facts carry identity only; route-wide facts are plan properties.
assert set(plan.endpoint(3)) == {
    "physical_endpoint_id", "logical_participant_id", "router_id", "router_port"}
assert plan.reduce_vn == 0 and plan.result_vn == 1
assert plan.processor_params(0)["reduce_vn"] == 0 and plan.processor_params(0)["result_vn"] == 1
shifted = StaticCollectivePlan(2, ROUTER_LINKS, ENDPOINT_LINKS, reduce_vn=2, result_vn=3)
assert shifted.reduce_vn == 2 and shifted.result_vn == 3
assert shifted.processor_params(2)["reduce_vn"] == 2 and shifted.processor_params(2)["result_vn"] == 3
assert StaticCollectivePlan.from_fattree(
    topology, reduce_vn=1, result_vn=2).processor_params(0)["result_vn"] == 2

three = topoFatTree()
three.shape = "2,1:2,1:2"
wide = StaticCollectivePlan.from_fattree(three)
assert wide.router_ids == (0, 1, 2, 3, 4, 5, 6) and wide.endpoint_ids == tuple(range(8))
assert wide.processor_params(6)["child_nids"] == [0, 4]
assert wide.processor_params(4)["parent_port"] == 2 and wide.processor_params(0)["parent_port"] == 2
assert wide.processor_params(5)["child_ports"] == [0, 1] and wide.processor_params(5)["child_nids"] == [4, 6]
one_branch = StaticCollectivePlan.from_fattree(three, logical_ids={7: 0})
assert one_branch.router_ids == (3, 5, 6) and one_branch.endpoint_ids == (7,)
assert one_branch.processor_params(6)["child_ports"] == [1]
assert one_branch.processor_params(5)["child_ports"] == [1]
assert one_branch.processor_params(3)["local_ports"] == [1]

# Several top-level routers: only the root's subtree carries processors.
fat = topoFatTree()
fat.shape = "4,4:4,4:8"
pruned = StaticCollectivePlan.from_fattree(fat)
assert len(pruned.router_ids) == 1 + 8 + 32 and len(pruned.endpoint_ids) == 128
assert 64 in pruned.router_ids and 65 not in pruned.router_ids and 33 not in pruned.router_ids
assert pruned.processor_params(64)["child_ports"] == list(range(8))
assert pruned.processor_params(32)["parent_port"] == 4 and pruned.processor_params(0)["parent_port"] == 4
try:
    StaticCollectivePlan.from_fattree(fat, root_router=200)
except ValueError:
    pass
else:
    raise RuntimeError("from_fattree accepted a router outside the fat tree")


# Inspect the actual topology builder with absent hosts. Explicit port checks
# guard against compaction; all described router links must also be constructed.
class Link:
    def __init__(self, name):
        self.name = name

    def setNoCut(self):
        pass


class Component:
    def __init__(self):
        self.links = {}

    def addLink(self, link, port, latency):
        self.links[port] = link.name

    def setSubComponent(self, *args):
        return self

    def addParams(self, params):
        pass


class SparseEndpoints:
    def build(self, node_id, extra_keys):
        return (Component(), "rtr") if node_id in (1, 7) else (None, None)


physical = topoFatTree()
physical.shape = "2,2:2,2:2"
physical.link_latency = "1ns"
physical.host_link_latency = "1ns"
routers = {}


def instance_router(topology, radix, router_id):
    routers[router_id] = Component()
    return routers[router_id]


with patch("sst.Link", Link), patch.object(topoFatTree, "_instanceRouter", instance_router):
    physical._build_impl(SparseEndpoints())
assert routers[0].links["port1"] == "hostlink_1" and "port0" not in routers[0].links
assert routers[3].links["port1"] == "hostlink_7" and "port0" not in routers[3].links
assert routers[5].links["port3"] == "link_l2_g0_r3_p0"
for upper, down_port, lower, up_port in physical.getConnectivity().router_links:
    assert routers[upper].links["port%d" % down_port] == routers[lower].links["port%d" % up_port]
for root in range(8, 12):
    selected = StaticCollectivePlan.from_fattree(physical, root_router=root, logical_ids={1: 0, 7: 1})
    assert selected.endpoint_ids == (1, 7)
    for router in selected.router_ids:
        fact = selected.processor_params(router)
        for port in fact["child_ports"] + fact["local_ports"]:
            assert "port%d" % port in routers[router].links

require_rejected(
    "a disconnected graph",
    router_links=((0, 2, 2, 0),),
    endpoint_links=ENDPOINT_LINKS + ((4, 4, 3, 0),),
)
require_rejected(
    "a cyclic graph",
    router_links=ROUTER_LINKS + ((0, 3, 1, 3),),
)
require_rejected(
    "a duplicate logical participant",
    endpoint_links=ENDPOINT_LINKS[:-1] + ((3, 2, 1, 1),),
)
# Fanout streams through bounded storage instead of reserving every branch.
timed = StaticCollectivePlan(2, ROUTER_LINKS, ENDPOINT_LINKS,
                            reduction_ops_per_cycle=2, reduction_latency_cycles=0)
assert timed.processor_params(2)["reduction_ops_per_cycle"] == 2
assert timed.processor_params(2)["reduction_latency_cycles"] == 0
require_rejected("zero reduction throughput", reduction_ops_per_cycle=0)
require_rejected("negative reduction latency", reduction_latency_cycles=-1)
require_rejected("identical service VNs", reduce_vn=1, result_vn=1)
require_rejected("a negative service VN", reduce_vn=-1)
require_rejected("a Boolean service VN", result_vn=True)
require_rejected("a Boolean route identifier", route_id=True)
require_rejected("an oversized namespace", job_namespace=1 << 64)
require_rejected("an oversized route identifier", route_id=1 << 64)
require_rejected("an oversized router identifier", root_router=1 << 31)
require_rejected(
    "an oversized router port",
    router_links=((0, (1 << 31) - 1, 2, 0), (1, 2, 2, 1)),
)
require_rejected(
    "an oversized physical endpoint identifier",
    endpoint_links=ENDPOINT_LINKS[:-1] + (((1 << 63), 3, 1, 1),),
)
require_rejected(
    "an oversized logical participant identifier",
    endpoint_links=ENDPOINT_LINKS[:-1] + ((3, (1 << 63), 1, 1),),
)
require_rejected("an oversized output depth", output_queue_depth=1 << 32)

print("StaticCollectivePlan contract PASS")
