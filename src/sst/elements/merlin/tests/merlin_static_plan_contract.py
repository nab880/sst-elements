# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.

from sst.merlin.collective import StaticCollectivePlan, StaticCollectiveRouter


ROUTER_LINKS = ((0, 2, 2, 0), (1, 2, 2, 1))
ENDPOINT_LINKS = tuple((nid, nid, nid // 2, nid % 2) for nid in range(4))


def require_rejected(message, root_router=2, router_links=ROUTER_LINKS,
                     endpoint_links=ENDPOINT_LINKS, **kwargs):
    try:
        StaticCollectivePlan(root_router, router_links, endpoint_links, **kwargs)
    except ValueError:
        return
    raise RuntimeError("StaticCollectivePlan accepted " + message)


plan = StaticCollectivePlan(2, ROUTER_LINKS, ENDPOINT_LINKS)
assert plan.router_ids == (0, 1, 2)
assert plan.endpoint_ids == (0, 1, 2, 3)
assert plan.endpoint(3)["logical_participant_id"] == 3
assert plan.processor_params(0)["parent_port"] == 2
assert plan.processor_params(2)["child_nids"] == [0, 2]
assert StaticCollectiveRouter(plan).clone()._collective_plan is plan

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
require_rejected("insufficient egress capacity", pending_egress_capacity=1)
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
require_rejected("an oversized pending capacity", pending_egress_capacity=1 << 32)
require_rejected("an oversized output depth", output_queue_depth=1 << 32)

print("StaticCollectivePlan contract PASS")
