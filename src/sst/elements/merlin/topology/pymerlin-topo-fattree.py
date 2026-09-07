#!/usr/bin/env python3
#
# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S.
# Government retains certain rights in this software.
#
# Copyright (c) 2009-2026, NTESS
# All rights reserved.
#
# Portions are copyright of other developers:
# See the file CONTRIBUTORS.TXT in the top level directory
# of the distribution for more information.
#
# This file is part of the SST software package. For license
# information, see the LICENSE file in the top level directory of the
# distribution.

import sst
from types import MappingProxyType
from sst.merlin.base import *


class FatTreeConnectivity:
    """Immutable physical router/endpoint ports for one fat-tree shape.

    Both construction and collective planning consume this description;
    router links are oriented from the upper router to the lower router.
    Router iteration retains the historical depth-first construction order.
    """

    def __init__(self, downs, ups, routers_per_level, groups_per_level, start_ids):
        per_group = tuple(routers // groups for routers, groups in
                          zip(routers_per_level, groups_per_level))
        routers = {}
        router_links = []
        endpoint_links = {}
        down_links = {}

        def router_id(level, group, index):
            return start_ids[level] + group * per_group[level] + index

        def describe_group(level, group):
            for index in range(per_group[level]):
                upper = router_id(level, group, index)
                down_links[upper] = []
                if level:
                    for port in range(downs[level]):
                        lower = router_id(level - 1, group * downs[level] + port,
                                          index % per_group[level - 1])
                        lower_port = downs[level - 1] + index // per_group[level - 1]
                        edge = (upper, port, lower, lower_port)
                        router_links.append(edge)
                        down_links[upper].append(edge)
                else:
                    endpoint_links[upper] = tuple(
                        (upper * downs[0] + port, upper, port)
                        for port in range(downs[0]))
            if level:
                for port in range(downs[level]):
                    describe_group(level - 1, group * downs[level] + port)
            radix = downs[level] + (ups[level] if level < len(ups) else 0)
            for index in range(per_group[level]):
                routers[router_id(level, group, index)] = ((level, group, index), radix)

        describe_group(len(downs) - 1, 0)
        self.routers = MappingProxyType(routers)
        self.router_links = tuple(router_links)
        self.endpoint_links = MappingProxyType(endpoint_links)
        self.down_links = MappingProxyType({router: tuple(edges)
                                            for router, edges in down_links.items()})
        self.default_root = start_ids[-1]

    def __deepcopy__(self, memo):
        memo[id(self)] = self
        return self


class topoFatTree(Topology):

    def __init__(self):
        Topology.__init__(self)
        self._declareClassVariables(["link_latency","host_link_latency","bundleEndpoints","_ups","_downs","_routers_per_level","_groups_per_level","_start_ids",
                                     "_total_hosts", "_connectivity"])
        self._declareParams("main",["shape","routing_alg","adaptive_threshold"])
        self._setCallbackOnWrite("shape",self._shape_callback)
        self._subscribeToPlatformParamSet("topology")


    def _shape_callback(self,variable_name,value):
        self._lockVariable(variable_name)
        shape = value

        # Process the shape
        self._ups = []
        self._downs = []
        self._routers_per_level = []
        self._groups_per_level = []
        self._start_ids = []

        levels = shape.split(":")

        for l in levels:
            links = l.split(",")

            self._downs.append(int(links[0]))
            if len(links) > 1:
                self._ups.append(int(links[1]))

        self._total_hosts = 1
        for i in self._downs:
            self._total_hosts *= i


        self._routers_per_level = [0] * len(self._downs)
        self._routers_per_level[0] = self._total_hosts // self._downs[0]
        for i in range(1,len(self._downs)):
            self._routers_per_level[i] = self._routers_per_level[i-1] * self._ups[i-1] // self._downs[i]

        self._start_ids = [0] * len(self._downs)
        for i in range(1,len(self._downs)):
            self._start_ids[i] = self._start_ids[i-1] + self._routers_per_level[i-1]

        self._groups_per_level = [1] * len(self._downs);
        if self._ups: # if ups is empty, then this is a single level and the following line will fail
            self._groups_per_level[0] = self._total_hosts // self._downs[0]

        for i in range(1,len(self._downs)-1):
            self._groups_per_level[i] = self._groups_per_level[i-1] // self._downs[i]

        self._connectivity = FatTreeConnectivity(
            tuple(self._downs), tuple(self._ups), tuple(self._routers_per_level),
            tuple(self._groups_per_level), tuple(self._start_ids))



    def getName(self):
        return "Fat Tree"



    def getNumNodes(self):
        return self._total_hosts

    def getConnectivity(self):
        """Return the immutable port description used to build this topology."""
        return self._connectivity


    def getRouterNameForId(self,rtr_id):
        num_levels = len(self._start_ids)

        # Check to make sure the index is in range
        level = num_levels - 1
        if rtr_id >= (self._start_ids[level] + self._routers_per_level[level]) or rtr_id < 0:
            print("ERROR: topoFattree.getRouterNameForId: rtr_id not found: %d"%rtr_id)
            sst.exit()

        # Find the level
        for x in range(num_levels-1,0,-1):
            if rtr_id >= self._start_ids[x]:
                break
            level = level - 1

        # Find the group
        remainder = rtr_id - self._start_ids[level]
        routers_per_group = self._routers_per_level[level] // self._groups_per_level[level]
        group = remainder // routers_per_group
        router = remainder % routers_per_group
        return self.getRouterNameForLocation((level,group,router))


    def getRouterNameForLocation(self,location):
        return "rtr_l%s_g%d_r%d"%(location[0],location[1],location[2])

    def findRouterByLocation(self,location):
        return sst.findComponentByName(self.getRouterNameForLocation(location));



    def _build_impl(self, endpoint):

        if not self.host_link_latency:
            self.host_link_latency = self.link_latency

        connectivity = self.getConnectivity()
        links_by_router = {router: [] for router in connectivity.routers}
        for upper, down_port, lower, up_port in connectivity.router_links:
            level, group, index = connectivity.routers[upper][0]
            link = sst.Link("link_l%d_g%d_r%d_p%d" % (level, group, index, down_port))
            links_by_router[upper].append((down_port, link))
            links_by_router[lower].append((up_port, link))

        for router_id, (location, radix) in connectivity.routers.items():
            for node_id, _, host_port in connectivity.endpoint_links.get(router_id, ()):
                ep, port_name = endpoint.build(node_id, {})
                if ep:
                    link = sst.Link("hostlink_%d" % node_id)
                    if self.bundleEndpoints:
                        link.setNoCut()
                    ep.addLink(link, port_name, self.host_link_latency)
                    # Preserve physical host port numbers when an endpoint is absent.
                    links_by_router[router_id].append((host_port, link))

            router = self._instanceRouter(radix, router_id)
            topology = router.setSubComponent(self.router.getTopologySlotName(), "merlin.fattree")
            self._applyStatisticsSettings(topology)
            topology.addParams(self._getGroupParams("main"))
            for port, link in sorted(links_by_router[router_id]):
                router.addLink(link, "port%d" % port, self.link_latency)
