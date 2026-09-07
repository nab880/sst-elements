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

import sys
import sst
from sst.merlin.base import *

class HgJob(Job):
    def __init__(self, job_id, numNodes, numCores = 1, nicsPerNode = 1):
        Job.__init__(self,job_id,numNodes * nicsPerNode)
        self._declareClassVariables(["_numCores","_nicsPerNode","node","nic","os","_params","_numNodes", "_collective_plan"])
        self._collective_plan = None
        self._numCores = numCores
        self._nicsPerNode = nicsPerNode
        self._numNodes = numNodes
        self.node = HgNode()
        self.os = HgOS()
        self.nic = HgNIC()

    def getName(self):
        return "HgJob"


    def useCollectivePlan(self, plan):
        """Bind every node of this job to a StaticCollectivePlan.

        Allocation must already match the plan's exact physical/logical
        membership, with one rank and NIC per node. The node's num_vns must exceed both service VNs
        and its ordinary and manager VNs must be different from them.
        """
        from sst.merlin.collective import StaticCollectivePlan
        if not isinstance(plan, StaticCollectivePlan):
            raise TypeError("plan must be a StaticCollectivePlan")
        if self._numCores != 1 or self._nicsPerNode != 1:
            raise ValueError("static collectives require one rank and NIC per Mercury node")
        plan.validate_job(self)
        self._collective_plan = plan
        self.nic.enable_static_collective = True
        self.nic.job_namespace = plan.job_namespace
        self.nic.route_id = plan.route_id
        self.nic.root_nid = plan.root_nid
        self.nic.root_logical_nid = plan.root_logical_nid
        self.node.reduce_vn = plan.reduce_vn
        self.node.result_vn = plan.result_vn

    def build(self, nodeID, extraKeys):
        if self._collective_plan:
            if self._first_build:
                self._collective_plan.validate_job(self)
            self._collective_plan.validate_participant(nodeID, self._nid_map[nodeID])
        self._check_first_build()
        logical_id = self._nid_map[nodeID]
        node = self.node.build(nodeID,logical_id,self._numNodes * self._numCores, self._numCores)
        os = self.os.build(node,"os_slot")
        nic = self.nic.build(node,"nic_slot")
        if self.nic._getGroupParams("params").get("enable_static_collective", False):
            nic.addParam("physical_endpoint_id", nodeID)
            nic.addParam("logical_participant_id", logical_id)

        # Build NetworkInterface
        networkif, port_name = self.network_interface.build(node,"link_control_slot",0,self.job_id,self.size,logical_id,True)

        return (networkif, port_name)

class HgNode(TemplateBase):

    def __init__(self):
        TemplateBase.__init__(self)
        self._declareParams("params",["name",
                                      "verbose",
                                      "negligible_compute_bytes",
                                      "parallelism",
                                      "frequency",
                                      "flow_mtu",
                                      "channel_bandwidth",
                                      "num_channels",
                                      "num_vns",
                                      "ordinary_vn",
                                      "manager_vn",
                                      "reduce_vn",
                                      "result_vn",
                                     ])
        self._subscribeToPlatformParamSet("node")

    def build(self,nid,lid,nranks,npernode):
        if self._check_first_build():
            sst.addGlobalParams("params_%s"%self._instance_name, self._getGroupParams("params"))
        node = sst.Component("node" + str(nid), self.name)
        node.addGlobalParamSet("params_%s"%self._instance_name)
        node.addParam("nodeID", nid)
        node.addParam("logicalID", lid)
        node.addParam("nranks", nranks)
        node.addParam("npernode", npernode)
        return node

class HgNIC(TemplateBase):
    def __init__(self):
        TemplateBase.__init__(self)
        self._declareParams("params",["verbose",
                                      "mtu",
                                      "enable_static_collective",
                                      "job_namespace",
                                      "route_id",
                                      "root_nid",
                                      "root_logical_nid",
                                      "physical_endpoint_id",
                                      "logical_participant_id",
                                      "collective_submit_delay_ns",
                                      "collective_completion_delay_ns",
                                     ])
        self._subscribeToPlatformParamSet("nic")

    def build(self,comp,slot):
        if self._check_first_build():
            sst.addGlobalParams("params_%s"%self._instance_name, self._getGroupParams("params"))
        nic = comp.setSubComponent(slot,"hg.nic")
        nic.addGlobalParamSet("params_%s"%self._instance_name)
        return nic

class HgOS(TemplateBase):

    def __init__(self):
        TemplateBase.__init__(self)
        self._declareParams("params",["name",
                                      "verbose",
                                      "ncores",
                                      "nsockets",
                                     ])
        self._declareParamsWithUserPrefix("params","app1",
                                          ["name",
                                           "argv",
                                           "exe_library_name",
                                           "libraries",
                                           "dependencies",
                                           "verbose",
                                           "post_rdma_delay",
                                           "post_header_delay",
                                           "poll_delay",
                                           "rdma_pin_latency",
                                           "rdma_page_delay",
                                           "rdma_page_size",
                                           "max_vshort_msg_size",
                                           "max_eager_msg_size",
                                           "use_put_window",
                                           "compute_library_access_width",
                                           "compute_library_loop_overhead",
                                           "enable_collective_offload",
                                          ],
                                          "app1.")
        self._subscribeToPlatformParamSet("operating_system")

    def build(self,comp,slot):
        if self._check_first_build():
            sst.addGlobalParams("params_%s"%self._instance_name, self._getGroupParams("params"))
        sub = comp.setSubComponent(slot,self.name)
        sub.addGlobalParamSet("params_%s"%self._instance_name)
        return sub
