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
        self._declareClassVariables(["_numCores","_nicsPerNode","node","nic","os","_params","_numNodes"])
        self._numCores = numCores
        self._nicsPerNode = nicsPerNode
        self._numNodes = numNodes
        self.node = HgNode()
        self.os = HgOS()
        self.nic = HgNIC()

    def getName(self):
        return "HgJob"


    def build(self, nodeID, extraKeys):
        logical_id = self._nid_map[nodeID]
        node = self.node.build(nodeID,logical_id,self._numNodes * self._numCores, self._numCores)
        os = self.os.build(node,"os_slot")
        nic = self.nic.build(node,"nic_slot")

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
                                     ])
        self._subscribeToPlatformParamSet("nic")

    def build(self,comp,slot):
        if self._check_first_build():
            sst.addGlobalParams("params_%s"%self._instance_name, self._getGroupParams("params"))
        nic = comp.setSubComponent(slot,"hg.nic")
        nic.addGlobalParamSet("params_%s"%self._instance_name)
        return nic

class HgOS(TemplateBase):

    def _forwardAppParam(self, key, value):
        if value is None:
            self._groups["params"].pop(key, None)
        else:
            self._groups["params"][key] = value

    def addParam(self, key, value):
        # app1.* keys that HgOS does not declare belong to the application:
        # collective selectors (allreduce is both a key and a prefix),
        # app1.env.* entries, and parameters read by hgcc pragmas such as
        # `#pragma sst overhead <param>` (app1.<param>). Forward them to the
        # component intact. TemplateBase would drop an undeclared variable
        # silently (KeyError) and reject an unknown level such as env with an
        # AttributeError (a platform param set prints that as a warning), so
        # declared-ness is checked here.
        if key.startswith("app1."):
            app_key = key[len("app1."):]
            app1 = self.__dict__.get("app1")
            declared = (app1 is not None and "." not in app_key
                        and app_key in app1._vars)
            if not declared:
                return self._forwardAppParam(key, value)
        return TemplateBase.addParam(self, key, value)

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
                                           "smp_optimize",
                                          ],
                                          "app1.")
        self._subscribeToPlatformParamSet("operating_system")

    def build(self,comp,slot):
        if self._check_first_build():
            sst.addGlobalParams("params_%s"%self._instance_name, self._getGroupParams("params"))
        sub = comp.setSubComponent(slot,self.name)
        sub.addGlobalParamSet("params_%s"%self._instance_name)
        return sub
