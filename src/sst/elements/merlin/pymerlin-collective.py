#!/usr/bin/env python3
#
# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S.
# Government retains certain rights in this software.
#
# This file is part of the SST software package. For license
# information, see the LICENSE file in the top level directory of the
# distribution.

from copy import deepcopy
from types import MappingProxyType

from sst.merlin.base import hr_router


class StaticCollectivePlan:
    """Validated static tree shared by routers and collective endpoints.

    Router links are ``(router, port, router, port)`` tuples. Endpoint links
    are ``(physical_nid, logical_id, router, port)`` tuples. The constructor
    validates the complete graph before any router installs its projection.
    """

    _INT_MAX = (1 << 31) - 1
    _MAX_ROUTER_PORT = _INT_MAX - 1
    _NID_MAX = (1 << 63) - 1
    _UINT32_MAX = (1 << 32) - 1
    _UINT64_MAX = (1 << 64) - 1

    def __init__(self, root_router, router_links, endpoint_links, *,
                 job_namespace=1, route_id=1,
                 pending_egress_capacity=None, egress_clock="1GHz",
                 output_queue_depth=1):
        common = {
            "job_namespace": self._bounded_id(
                "job_namespace", job_namespace, self._UINT64_MAX, nonzero=True),
            "route_id": self._bounded_id("route_id", route_id, self._UINT64_MAX),
        }
        if not isinstance(egress_clock, str) or not egress_clock:
            raise ValueError("egress_clock must be a nonempty string")
        output_queue_depth = self._bounded_id(
            "output_queue_depth", output_queue_depth, self._UINT32_MAX, nonzero=True)
        if pending_egress_capacity is not None:
            pending_egress_capacity = self._bounded_id(
                "pending_egress_capacity", pending_egress_capacity,
                self._UINT32_MAX, nonzero=True)

        root_router = self._bounded_id("root_router", root_router, self._INT_MAX)
        adjacency = {root_router: []}
        used_ports = set()
        declared_router_links = set()
        edge_count = 0
        for entry in router_links:
            if len(entry) != 4:
                raise ValueError("router links must contain four integers")
            left = self._bounded_id("router ID", entry[0], self._INT_MAX)
            left_port = self._bounded_id(
                "router port", entry[1], self._MAX_ROUTER_PORT)
            right = self._bounded_id("router ID", entry[2], self._INT_MAX)
            right_port = self._bounded_id(
                "router port", entry[3], self._MAX_ROUTER_PORT)
            if left == right:
                raise ValueError("a collective router cannot link to itself")
            for port in ((left, left_port), (right, right_port)):
                if port in used_ports:
                    raise ValueError("router port %d:%d is used more than once" % port)
                used_ports.add(port)
            adjacency.setdefault(left, []).append((right, left_port, right_port))
            adjacency.setdefault(right, []).append((left, right_port, left_port))
            declared_router_links.add(
                self._canonical_router_link(left, left_port, right, right_port))
            edge_count += 1

        endpoints = {}
        logical_ids = set()
        local = {}
        declared_endpoint_links = set()
        for entry in endpoint_links:
            if len(entry) != 4:
                raise ValueError("endpoint links must contain four integers")
            physical = self._bounded_id("physical endpoint ID", entry[0], self._NID_MAX)
            logical = self._bounded_id("logical participant ID", entry[1], self._NID_MAX)
            router = self._bounded_id("router ID", entry[2], self._INT_MAX)
            port = self._bounded_id("router port", entry[3], self._MAX_ROUTER_PORT)
            if physical in endpoints:
                raise ValueError("physical endpoint %d is present more than once" % physical)
            if logical in logical_ids:
                raise ValueError("logical participant %d is present more than once" % logical)
            if (router, port) in used_ports:
                raise ValueError("router port %d:%d is used more than once" % (router, port))
            used_ports.add((router, port))
            logical_ids.add(logical)
            endpoints[physical] = (logical, router, port)
            declared_endpoint_links.add((physical, router, port))
            local.setdefault(router, []).append((port, physical, logical))
            adjacency.setdefault(router, [])
        if not endpoints:
            raise ValueError("a static collective requires at least one endpoint")

        parent = {root_router: None}
        order = [root_router]
        for router in order:
            for neighbor, local_port, remote_port in adjacency[router]:
                if parent[router] is not None and neighbor == parent[router][0]:
                    continue
                if neighbor in parent:
                    raise ValueError("collective router links contain a cycle")
                parent[neighbor] = (router, remote_port, local_port)
                order.append(neighbor)
        if len(parent) != len(adjacency) or edge_count != len(adjacency) - 1:
            raise ValueError("collective router links must form one connected tree")

        children = {router: [] for router in adjacency}
        for child, parent_info in parent.items():
            if parent_info is None:
                continue
            children[parent_info[0]].append((parent_info[2], child))

        representatives = {}
        for router in reversed(order):
            candidates = [(physical, logical)
                          for _, physical, logical in local.get(router, ())]
            candidates.extend(representatives[child] for _, child in children[router])
            if not candidates:
                raise ValueError("router %d has no participant in its subtree" % router)
            representatives[router] = min(candidates)
        root_nid, root_logical_nid = representatives[root_router]

        router_facts = {}
        for router in order:
            local_branches = sorted(local.get(router, ()))
            child_branches = sorted(children[router])
            branch_count = len(local_branches) + len(child_branches)
            if branch_count > self._UINT32_MAX:
                raise ValueError("router %d has too many collective branches" % router)
            capacity = branch_count if pending_egress_capacity is None else pending_egress_capacity
            if capacity < branch_count:
                raise ValueError(
                    "router %d needs %d pending egress slots, not %d" %
                    (router, branch_count, capacity))
            fact = dict(common)
            fact.update({
                "root": router == root_router,
                "parent_port": -1 if parent[router] is None else parent[router][1],
                "child_ports": tuple(port for port, _ in child_branches),
                "child_nids": tuple(representatives[child][0] for _, child in child_branches),
                "child_logical_nids": tuple(
                    representatives[child][1] for _, child in child_branches),
                "local_ports": tuple(port for port, _, _ in local_branches),
                "local_nids": tuple(physical for _, physical, _ in local_branches),
                "local_logical_nids": tuple(logical for _, _, logical in local_branches),
                "root_nid": root_nid,
                "root_logical_nid": root_logical_nid,
                "subtree_nid": representatives[router][0],
                "subtree_logical_nid": representatives[router][1],
                "pending_egress_capacity": capacity,
                "egress_clock": egress_clock,
                "required_radix": 1 + max(port for owner, port in used_ports if owner == router),
            })
            router_facts[router] = MappingProxyType(fact)

        endpoint_facts = {}
        for physical, (logical, router, port) in endpoints.items():
            endpoint_facts[physical] = MappingProxyType({
                "physical_endpoint_id": physical,
                "logical_participant_id": logical,
                "router_id": router,
                "router_port": port,
                "root_nid": root_nid,
                "root_logical_nid": root_logical_nid,
                "job_namespace": common["job_namespace"],
                "route_id": common["route_id"],
                "participant_slot": 0,
                "reduce_vn": 0,
                "result_vn": 1,
            })

        self._routers = MappingProxyType(router_facts)
        self._endpoints = MappingProxyType(endpoint_facts)
        self._root_nid = root_nid
        self._root_logical_nid = root_logical_nid
        self._job_namespace = common["job_namespace"]
        self._route_id = common["route_id"]
        self._output_queue_depth = output_queue_depth
        self._declared_router_links = frozenset(declared_router_links)
        self._declared_endpoint_links = frozenset(declared_endpoint_links)

    def __deepcopy__(self, memo):
        # Plans contain only immutable mappings and tuples.  Merlin templates
        # are cloned with deepcopy, so share the validated plan itself.
        memo[id(self)] = self
        return self

    @staticmethod
    def _bounded_id(name, value, maximum, nonzero=False):
        minimum = 1 if nonzero else 0
        if (isinstance(value, bool) or not isinstance(value, int) or
                value < minimum or value > maximum):
            raise ValueError(
                "%s must be a %s integer no greater than %d" %
                (name, "positive" if nonzero else "nonnegative", maximum))
        return value

    @staticmethod
    def _canonical_router_link(left, left_port, right, right_port):
        left_endpoint = (left, left_port)
        right_endpoint = (right, right_port)
        if right_endpoint < left_endpoint:
            left_endpoint, right_endpoint = right_endpoint, left_endpoint
        return left_endpoint + right_endpoint

    def validate_built_topology(self, router_links, endpoint_links):
        """Require every declared collective attachment in a built topology."""
        actual_router_links = frozenset(
            self._canonical_router_link(*entry) for entry in router_links)
        actual_endpoint_links = frozenset(endpoint_links)
        missing_router_links = self._declared_router_links - actual_router_links
        missing_endpoint_links = self._declared_endpoint_links - actual_endpoint_links
        if not missing_router_links and not missing_endpoint_links:
            return

        missing = []
        if missing_router_links:
            missing.append("router links %s" % sorted(missing_router_links))
        if missing_endpoint_links:
            missing.append("endpoint links %s" % sorted(missing_endpoint_links))
        raise ValueError(
            "static collective plan does not match built fat-tree; missing " +
            "; ".join(missing))

    @property
    def router_ids(self):
        return tuple(sorted(self._routers))

    @property
    def endpoint_ids(self):
        return tuple(sorted(self._endpoints))

    @property
    def root_nid(self):
        return self._root_nid

    @property
    def root_logical_nid(self):
        return self._root_logical_nid

    @property
    def job_namespace(self):
        return self._job_namespace

    @property
    def route_id(self):
        return self._route_id

    def router(self, router_id):
        return self._routers[router_id]

    def endpoint(self, physical_endpoint_id):
        return self._endpoints[physical_endpoint_id]

    def processor_params(self, router_id):
        """Return a fresh SST parameter dictionary for one validated router."""
        return {name: list(value) if isinstance(value, tuple) else value
                for name, value in self.router(router_id).items()
                if name != "required_radix"}


class StaticCollectiveRouter(hr_router):
    """Merlin router template that installs projections from one plan."""

    _processor_statistics = (
        "local_contributions", "child_contributions", "parent_results",
        "upward_aggregates", "result_packets", "active_high_water",
        "installed_branch_slots", "egress_retries",
    )

    def __init__(self, plan, enable_statistics=False):
        super().__init__()
        if not isinstance(plan, StaticCollectivePlan):
            raise TypeError("plan must be a StaticCollectivePlan")
        self._declareClassVariables(["_collective_plan", "_collective_statistics"])
        self._collective_plan = plan
        self._collective_statistics = enable_statistics

    def __deepcopy__(self, memo):
        clone = self.__class__.__new__(self.__class__)
        memo[id(self)] = clone
        for name, value in self.__dict__.items():
            object.__setattr__(clone, name, deepcopy(value, memo))
        return clone

    def _validate_collective_topology(self, router_links, endpoint_links):
        self._collective_plan.validate_built_topology(router_links, endpoint_links)

    def instanceRouter(self, name, radix, router_id):
        fact = self._collective_plan.router(router_id)
        if radix < fact["required_radix"]:
            raise ValueError("router %d has radix %d, but its collective plan needs %d" %
                             (router_id, radix, fact["required_radix"]))
        router = super().instanceRouter(name, radix, router_id)
        router.addParam("network_service_output_queue_depth",
                        self._collective_plan._output_queue_depth)
        processor = router.setSubComponent(
            "network_service", "merlin.collective_static_processor")
        processor.addParams(self._collective_plan.processor_params(router_id))
        if self._collective_statistics:
            processor.enableStatistics(
                list(self._processor_statistics), {"type": "sst.AccumulatorStatistic"})
            router.enableStatistics(
                ["network_service_accept", "network_service_synthetic"],
                {"type": "sst.AccumulatorStatistic"})
        return router
