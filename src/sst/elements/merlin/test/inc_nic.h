// -*- mode: c++ -*-

// Copyright 2009-2025 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2025, NTESS
// All rights reserved.
//
// Portions are copyright of other developers:
// See the file CONTRIBUTORS.TXT in the top level directory
// of the distribution for more information.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.


#ifndef COMPONENTS_MERLIN_INC_NIC_H
#define COMPONENTS_MERLIN_INC_NIC_H

#include <sst/core/component.h>
#include <sst/core/event.h>
#include <sst/core/link.h>
#include <sst/core/timeConverter.h>
#include <sst/core/interfaces/simpleNetwork.h>
#include <router.h>


namespace SST {

namespace Merlin {


class inc_nic : public Component {

public:

    SST_ELI_REGISTER_COMPONENT(
        inc_nic,
        "merlin",
        "inc_nic",
        SST_ELI_ELEMENT_VERSION(1,0,0),
        "Simple NIC to test base INC functionality.",
        COMPONENT_CATEGORY_NETWORK)

    SST_ELI_DOCUMENT_PARAMS(
        {"job_id",       "ID of INC job."},
        {"id",           "Network ID of endpoint."},
        {"next_ports"    "Port number of next port on this router for this INC job."},
        {"root_ports"    "Port number of root port on this router for this INC job."},
        {"up_ports"      "Port number of next higher-level switch for this INC job."},
        {"message_size", "Size of each message to be sent specified in either b or B (can include SI prefix)."},
        {"num_messages", "Number of messages to be sent"}
    )

    SST_ELI_DOCUMENT_PORTS(
    )

    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        {"networkIF", "Network interface", "SST::Interfaces::SimpleNetwork" }
    )

private:

    int job_id;
    int net_id;
    std::vector<int> next_ports;
    std::vector<int> root_ports;
    std::vector<int> up_ports;
    int msg_size;
    int num_msg;

    int packets_sent;
    int packets_recd;
    int stalled_cycles;

    bool done;

    SST::Interfaces::SimpleNetwork* link_control;

    Output& output;

public:
    inc_nic(ComponentId_t cid, Params& params);
    ~inc_nic();

    void init(unsigned int phase);
    void complete(unsigned int phase);
    void setup();
    void finish();

private:
    bool clock_handler(Cycle_t cycle);

};

}
}

#endif // COMPONENTS_MERLIN_INC_NIC_H
