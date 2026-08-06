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
from sst.merlin.base import *


class TestJob(Job):
    def __init__(self, job_id, size):
        Job.__init__(self, job_id, size)
        self._declareParams(
            "main", ["num_peers", "num_messages", "message_size", "send_untimed_bcast"]
        )
        self.num_peers = size
        self._lockVariable("num_peers")

    def getName(self):
        return "TestJob"

    def build(self, nID, extraKeys, link=None):
        nic = sst.Component("testNic_%d" % nID, "merlin.test_nic")
        self._applyStatisticsSettings(nic)
        nic.addParams(self._getGroupParams("main"))
        nic.addParams(extraKeys)
        # Get the logical node id
        id = self._nid_map[nID]
        nic.addParam("id", id)

        #  Add the linkcontrol
        return NetworkInterface._instanceNetworkInterfaceBackCompat(
            self.network_interface,
            nic,
            "networkIF",
            0,
            self.job_id,
            self.size,
            id,
            True,
            link,
        )


class INCJob(Job):
    def __init__(self, job_id, size):
        Job.__init__(self, job_id, size)
        self._declareParams("main", ["job_id", "message_size", "num_messages", "shape"])
        self.job_id = job_id
        self._lockVariable("job_id")

    def getName(self):
        return "INCJob"

    def build(self, nID, extraKeys, link=None):
        nic = sst.Component("incNic_%d" % nID, "merlin.inc_nic")
        self._applyStatisticsSettings(nic)
        nic.addParams(self._getGroupParams("main"))
        nic.addParams(extraKeys)
        # Get the logical node id
        id = self._nid_map[nID]
        nic.addParam("id", id)

        downs = [int(d.split(",")[0]) for d in self.shape.split(":")]
        ups = [int(d.split(",")[1]) for d in self.shape.split(":") if "," in d]

        next_ports = []
        root_ports = []
        up_ports = []

        num_hosts = 1
        for l in range(len(downs)):
            num_hosts *= downs[l]

        num_switches = [num_hosts // downs[0]]
        for l in range(1, len(downs)):
            num_switches.append(num_switches[-1] * ups[l - 1] // downs[l])

        num_groups = [num_hosts // downs[0]]
        for l in range(1, len(downs)):
            num_groups.append(num_groups[-1] // downs[l])

        num_switches_per_group = [s // g for (s, g) in zip(num_switches, num_groups)]

        all_nIDs = sorted(list(self._nid_map.keys()))

        level_nID = nID

        for l in range(len(downs)):
            port_map = [
                s
                // (num_switches_per_group[l] * downs[l])
                * (num_switches_per_group[l] * downs[l])
                + s % num_switches_per_group[l] * num_switches_per_group[l]
                + s
                % (num_switches_per_group[l] * downs[l])
                // num_switches_per_group[l]
                for s in range(num_switches[l] * downs[l])
            ]
            all_nIDs = [port_map[p] for p in all_nIDs]

            all_ports = [[] for s in range(num_switches[l])]
            for p in all_nIDs:
                all_ports[p // downs[l]].append(p % downs[l])

            level_switch = port_map[level_nID] // downs[l]
            level_port = port_map[level_nID] % downs[l]

            next_ports.append(
                all_ports[level_switch][
                    (all_ports[level_switch].index(level_port) + 1)
                    % len(all_ports[level_switch])
                ]
            )
            root_ports.append(all_ports[level_switch][0])

            if all_ports[level_switch].index(level_port) > 0:
                break

            if l < len(ups):
                # up_ports.append(downs[l]+self.job_id%ups[l])
                up_ports.append(downs[l] + 0)

                all_nIDs = [
                    s * ups[l] + self.job_id % ups[l]
                    for s in range(num_switches[l])
                    if len(all_ports[s]) > 0
                ]
                all_nIDs = sorted(list(set(all_nIDs)))

                level_nID = level_switch * ups[l] + self.job_id % ups[l]

        nic.addParam("next_ports", next_ports)
        nic.addParam("root_ports", root_ports)
        nic.addParam("up_ports", up_ports)

        return NetworkInterface._instanceNetworkInterfaceBackCompat(
            self.network_interface,
            nic,
            "networkIF",
            0,
            self.job_id,
            self.size,
            id,
            True,
            link,
        )


class OfferedLoadJob(Job):
    def __init__(self, job_id, size):
        Job.__init__(self, job_id, size)
        self._declareParams(
            "main",
            [
                "offered_load",
                "num_peers",
                "message_size",
                "link_bw",
                "warmup_time",
                "collect_time",
                "drain_time",
            ],
        )
        self._declareClassVariables(["pattern"])
        self.num_peers = size
        self._lockVariable("num_peers")

    def getName(self):
        return "Offered Load Job"

    def build(self, nID, extraKeys):
        nic = sst.Component("offered_load_%d" % nID, "merlin.offered_load")
        self._applyStatisticsSettings(nic)
        nic.addParams(self._getGroupParams("main"))
        nic.addParams(extraKeys)
        id = self._nid_map[nID]
        nic.addParam("id", id)

        # Add pattern generator
        self.pattern.addAsAnonymous(nic, "pattern", "pattern.")

        #  Add the linkcontrol
        networkif, port_name = self.network_interface.build(
            nic, "networkIF", 0, self.job_id, self.size, id, True
        )

        return (networkif, port_name)


class IncastJob(Job):
    def __init__(self, job_id, size):
        Job.__init__(self, job_id, size)
        self._declareParams(
            "main",
            [
                "num_peers",
                "target_nids",
                "packets_to_send",
                "packet_size",
                "delay_start",
            ],
        )
        self.num_peers = size
        self._lockVariable("num_peers")

    def getName(self):
        return "Incast Job"

    def build(self, nID, extraKeys):
        nic = sst.Component("incast_%d" % nID, "merlin.simple_patterns.incast")
        self._applyStatisticsSettings(nic)
        nic.addParams(self._getGroupParams("main"))
        nic.addParams(extraKeys)
        id = self._nid_map[nID]

        #  Add the linkcontrol
        networkif, port_name = self.network_interface.build(
            nic, "networkIF", 0, self.job_id, self.size, id, True
        )
        return (networkif, port_name)
