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
#include <sst_config.h>
#include "sst/elements/merlin/test/inc_nic.h"

#include "sst/elements/merlin/merlin.h"

#include <unistd.h>
#include <signal.h>

#include <sst/core/event.h>
#include <sst/core/params.h>
#include <sst/core/timeLord.h>
#include <sst/core/unitAlgebra.h>

#include <sst/core/interfaces/simpleNetwork.h>

namespace SST {
using namespace SST::Interfaces;

namespace Merlin {

inc_nic::inc_nic(ComponentId_t cid, Params& params) :
    Component(cid),
    packets_sent(0),
    packets_recd(0),
    stalled_cycles(0),
    done(false),
    output(getSimulationOutput())
{
    job_id = params.find<int>("job_id",-1);
    net_id = params.find<int>("id",-1);

    params.find_array<int>("next_ports", next_ports);
    params.find_array<int>("root_ports", root_ports);
    params.find_array<int>("up_ports", up_ports);

    UnitAlgebra message_size = params.find<std::string>("message_size","64b");
    if ( message_size.hasUnits("B") ) message_size  *= UnitAlgebra("8b/B");
    msg_size = message_size.getRoundedValue();

    num_msg = params.find<int>("num_messages", 1);

    link_control = loadUserSubComponent<SST::Interfaces::SimpleNetwork>("networkIF", ComponentInfo::SHARE_NONE, 1);
    if ( !link_control ) {
        merlin_abort.fatal(CALL_INFO,1,"Error: no LinkControl object loaded into inc_nic\n");
    }

    registerClock( "1GHz", new Clock::Handler<inc_nic,&inc_nic::clock_handler>(this), false);

    registerAsPrimaryComponent();
    primaryComponentDoNotEndSim();
}


inc_nic::~inc_nic() {
    delete link_control;
}

void inc_nic::finish() {
    link_control->finish();
}

void inc_nic::setup() {
    link_control->setup();
    if ( link_control->getEndpointID() != net_id ) {
        output.output("NIC ids don't match: param = %" PRIi64 ", LinkControl = %" PRIi64 "\n", (int64_t) net_id, (int64_t) link_control->getEndpointID());
    }
}

void inc_nic::complete(unsigned int phase) {
    link_control->complete(phase);
}

void inc_nic::init(unsigned int phase) {
    link_control->init(phase);
}

bool inc_nic::clock_handler(Cycle_t cycle) {
    if ( !done && (packets_recd >= num_msg) ) {
        primaryComponentOKToEndSim();
        done = true;
        return true;
    }

    if ( packets_sent < num_msg ) {
        if ( link_control->spaceToSend(0, msg_size) ) {
            incEvent* ev = new incEvent(packets_sent, net_id, next_ports, root_ports, up_ports);
            //incEvent* ev = new incEvent(job_id, net_id, next_ports, root_ports, up_ports);
            SimpleNetwork::Request* req = new SimpleNetwork::Request();

            req->dest = net_id;
            req->src = net_id;

            req->vn = 0;
            req->size_in_bits = msg_size;
            req->givePayload(ev);

            link_control->send(req, 0);

            packets_sent++;
        }
        else {
            stalled_cycles++;
        }
    }

    if ( link_control->requestToReceive(0) ) {
        SimpleNetwork::Request* req = link_control->recv(0);
        incEvent* ev = dynamic_cast<incEvent*>(req->takePayload());
        if ( !ev ) {
            output.fatal(CALL_INFO, -1, "Error: Received event of wrong type!\n");
        }
        // std::cout << "[" << ev->job_id << "." << net_id << "] = " << ev->data << std::endl;
        packets_recd++;

        if ( req->dest != net_id ) {
            output.fatal(CALL_INFO,-1,"%d received packet intended for %d\n",net_id,(int)req->dest);
        }

        delete ev;
        delete req;
    }

    return false;
}

} // namespace Merlin
} // namespace SST
