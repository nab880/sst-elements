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

from copy import deepcopy
from collections.abc import Mapping
from types import MappingProxyType

from sst.merlin.base import hr_router


class StaticCollectivePlan:
    """Validated static tree shared by routers and collective endpoints.

    Use ``from_fattree`` to derive the tree from a ``topoFatTree``.  The
    constructor takes explicit router links as ``(router, port, router,
    port)`` tuples and endpoint links as ``(physical_nid, logical_id,
    router, port)`` tuples, and validates the complete graph before any
    router installs its projection.  Routers check the transport facts the
    model cannot see (connectivity, flit size, credits) during init and
    reject a plan that does not match the built network there.

    ``reduce_vn`` carries contributions toward the root and ``result_vn``
    carries results back; every router in the tree owns both VNs, so
    ordinary traffic must use other VNs.  Endpoint stacks bind a job to the
    plan with ``job.useCollectivePlan(plan)``.
    """

    _INT_MAX = (1 << 31) - 1
    _MAX_ROUTER_PORT = _INT_MAX - 1
    _NID_MAX = (1 << 63) - 1
    _UINT32_MAX = (1 << 32) - 1
    _UINT64_MAX = (1 << 64) - 1

    def __init__(self, root_router, router_links, endpoint_links, *,
                 job_namespace=1, route_id=1, reduce_vn=0, result_vn=1,
                 output_queue_depth=1,
                 reduction_ops_per_cycle=1, reduction_latency_cycles=1):
        reduce_vn = self._bounded_id("reduce_vn", reduce_vn, self._INT_MAX)
        result_vn = self._bounded_id("result_vn", result_vn, self._INT_MAX)
        if reduce_vn == result_vn:
            raise ValueError("reduce_vn and result_vn must be different VNs")
        common = {
            "job_namespace": self._bounded_id(
                "job_namespace", job_namespace, self._UINT64_MAX, nonzero=True),
            "route_id": self._bounded_id("route_id", route_id, self._UINT64_MAX),
            "reduce_vn": reduce_vn,
            "result_vn": result_vn,
            "reduction_ops_per_cycle": self._bounded_id(
                "reduction_ops_per_cycle", reduction_ops_per_cycle,
                self._UINT32_MAX, nonzero=True),
            "reduction_latency_cycles": self._bounded_id(
                "reduction_latency_cycles", reduction_latency_cycles, self._UINT32_MAX),
        }
        output_queue_depth = self._bounded_id(
            "output_queue_depth", output_queue_depth, self._UINT32_MAX, nonzero=True)

        root_router = self._bounded_id("root_router", root_router, self._INT_MAX)
        adjacency = {root_router: []}
        used_ports = set()
        required_radix = {}

        def use_port(router, port):
            if (router, port) in used_ports:
                raise ValueError("router port %d:%d is used more than once" % (router, port))
            used_ports.add((router, port))
            required_radix[router] = max(required_radix.get(router, 0), port + 1)

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
            use_port(left, left_port)
            use_port(right, right_port)
            adjacency.setdefault(left, []).append((right, left_port, right_port))
            adjacency.setdefault(right, []).append((left, right_port, left_port))
            edge_count += 1

        endpoints = {}
        logical_ids = set()
        local = {}
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
            use_port(router, port)
            logical_ids.add(logical)
            endpoints[physical] = (logical, router, port)
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
                "required_radix": required_radix[router],
            })
            router_facts[router] = MappingProxyType(fact)

        endpoint_facts = {}
        for physical, (logical, router, port) in endpoints.items():
            endpoint_facts[physical] = MappingProxyType({
                "physical_endpoint_id": physical,
                "logical_participant_id": logical,
                "router_id": router,
                "router_port": port,
            })

        self._routers = MappingProxyType(router_facts)
        self._endpoints = MappingProxyType(endpoint_facts)
        self._root_nid = root_nid
        self._root_logical_nid = root_logical_nid
        self._job_namespace = common["job_namespace"]
        self._route_id = common["route_id"]
        self._reduce_vn = reduce_vn
        self._result_vn = result_vn
        self._output_queue_depth = output_queue_depth

    def __deepcopy__(self, memo):
        # Plans contain only immutable mappings and tuples.  Merlin templates
        # are cloned with deepcopy, so share the validated plan itself.
        memo[id(self)] = self
        return self

    @classmethod
    def from_fattree(cls, topology, root_router=None, logical_ids=None, **kwargs):
        """Compile a participant tree from the topology's physical ports.

        ``logical_ids`` is an allocated job, a physical-to-logical mapping,
        or a sequence indexed by physical ID. A mapping selects exactly its
        endpoints; branches with no selected participant are pruned. Without
        a mapping, all hosts below the chosen root participate. The default
        root is the first top-level router. Allocated endpoints outside a
        selected root's subtree are rejected, never silently omitted.
        """
        connectivity = topology.getConnectivity()
        if root_router is None:
            root_router = connectivity.default_root
        root_router = cls._bounded_id("root_router", root_router, cls._INT_MAX)
        if root_router not in connectivity.routers:
            raise ValueError("root_router %d is not a router of this fat tree" % root_router)
        membership = None if logical_ids is None else cls._membership(logical_ids)
        if membership is not None and not membership:
            raise ValueError("a static collective requires at least one allocated endpoint")

        order = [root_router]
        parent_edges = {}
        endpoint_links = []
        occupied = set()
        for router in order:
            for edge in connectivity.down_links[router]:
                parent_edges[edge[2]] = edge
                order.append(edge[2])
            for physical, _, port in connectivity.endpoint_links.get(router, ()):
                if membership is None or physical in membership:
                    logical = physical if membership is None else membership[physical]
                    endpoint_links.append((physical, logical, router, port))
                    occupied.add(router)

        selected = {physical for physical, _, _, _ in endpoint_links}
        if membership is not None and selected != membership.keys():
            missing = sorted(membership.keys() - selected)
            raise ValueError("allocated endpoints %s are outside root_router %d's subtree" %
                             (missing, root_router))
        router_links = []
        for router in reversed(order):
            if router in occupied and router != root_router:
                edge = parent_edges[router]
                router_links.append(edge)
                occupied.add(edge[0])
        return cls(root_router, router_links, endpoint_links, **kwargs)

    @classmethod
    def _membership(cls, source):
        if hasattr(source, "_nid_map"):
            source = source._nid_map
            if source is None:
                raise ValueError("collective job has not been allocated yet")
        if source is None:
            raise ValueError("collective job has not been allocated yet")
        entries = source.items() if isinstance(source, Mapping) else enumerate(source)
        return {cls._bounded_id("physical endpoint ID", physical, cls._NID_MAX):
                cls._bounded_id("logical participant ID", logical, cls._NID_MAX)
                for physical, logical in entries}

    def validate_job(self, job):
        """Require exact physical membership and rank order before building a job."""
        membership = self._membership(job)
        if len(membership) != job.getSize() or set(membership.values()) != set(range(job.getSize())):
            raise ValueError("collective job allocation must contain every logical rank exactly once")
        if membership.keys() != self._endpoints.keys():
            raise ValueError("collective plan physical membership does not match the job allocation")
        if any(logical != self._endpoints[physical]["logical_participant_id"]
               for physical, logical in membership.items()):
            raise ValueError("collective plan logical membership does not match the job allocation")

    def validate_participant(self, physical, logical):
        """Check each endpoint against the bound plan as it is constructed."""
        if physical not in self._endpoints or self._endpoints[physical]["logical_participant_id"] != logical:
            raise ValueError("collective participant does not match the bound job allocation")

    @staticmethod
    def _bounded_id(name, value, maximum, nonzero=False):
        minimum = 1 if nonzero else 0
        if (isinstance(value, bool) or not isinstance(value, int) or
                value < minimum or value > maximum):
            raise ValueError(
                "%s must be a %s integer no greater than %d" %
                (name, "positive" if nonzero else "nonnegative", maximum))
        return value

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

    @property
    def reduce_vn(self):
        return self._reduce_vn

    @property
    def result_vn(self):
        return self._result_vn

    def router(self, router_id):
        return self._routers[router_id]

    def endpoint(self, physical_endpoint_id):
        """Return the identity of one participant: its physical and logical
        IDs and the edge router port it hangs off."""
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
        self.network_service_output_queue_depth = plan._output_queue_depth
        self._collective_statistics = enable_statistics

    def __deepcopy__(self, memo):
        clone = self.__class__.__new__(self.__class__)
        memo[id(self)] = clone
        for name, value in self.__dict__.items():
            object.__setattr__(clone, name, deepcopy(value, memo))
        return clone

    def instanceRouter(self, name, radix, router_id):
        if router_id not in self._collective_plan._routers:
            # Routers outside the collective tree carry ordinary traffic only.
            return super().instanceRouter(name, radix, router_id)
        fact = self._collective_plan.router(router_id)
        if radix < fact["required_radix"]:
            raise ValueError("router %d has radix %d, but its collective plan needs %d" %
                             (router_id, radix, fact["required_radix"]))
        router = super().instanceRouter(name, radix, router_id)
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
