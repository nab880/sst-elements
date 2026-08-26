// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.
//
// Portions are copyright of other developers:
// See the file CONTRIBUTORS.TXT in the top level directory
// of the distribution for more information.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.


#ifndef COMPONENTS_HR_ROUTER_XBAR_ARB_RR_H
#define COMPONENTS_HR_ROUTER_XBAR_ARB_RR_H

#include <sst/core/component.h>
#include <sst/core/event.h>
#include <sst/core/link.h>
#include <sst/core/timeConverter.h>

#include <vector>

#include "sst/elements/merlin/router.h"

using namespace SST;

namespace SST {
namespace Merlin {

class xbar_arb_rr : public XbarArbitration {

public:

    SST_ELI_REGISTER_SUBCOMPONENT(
        xbar_arb_rr,
        "merlin",
        "xbar_arb_rr",
        SST_ELI_ELEMENT_VERSION(1,0,0),
        "Round robin arbitration unit for hr_router",
        SST::Merlin::XbarArbitration
    )


private:
    int num_ports = 0;
    int num_output_ports = 0;
    int num_vcs = 0;

    int* rr_vcs = nullptr;
    int rr_port = 0;
    int service_rr_port = 0;

#if VERIFY_DECLOCKING
    int rr_port_shadow = 0;
#endif

    internal_router_event** vc_heads = nullptr;

    // PortControl** ports;

public:

    xbar_arb_rr() = default;

    xbar_arb_rr(ComponentId_t cid, Params& params) :
        XbarArbitration(cid),
        rr_vcs(NULL)
    {
    }

    ~xbar_arb_rr() {
        if ( rr_vcs != NULL ) delete [] rr_vcs;
    }

    void serialize_order(SST::Core::Serialization::serializer& ser) override {
        XbarArbitration::serialize_order(ser);
        SST_SER(num_ports);
        SST_SER(num_output_ports);
        SST_SER(num_vcs);
        SST_SER(rr_port);
        SST_SER(service_rr_port);
#if VERIFY_DECLOCKING
        SST_SER(rr_port_shadow);
#endif

        SST_SER(SST::Core::Serialization::array(rr_vcs, num_ports));

        // vc_heads is a non-owning scratch alias set during arbitration.
        if ( ser.mode() == SST::Core::Serialization::serializer::UNPACK ) vc_heads = nullptr;
    }
    ImplementSerializable(SST::Merlin::xbar_arb_rr)

    void setPorts(int num_ports_s, int num_vcs_s) override
    {
        num_ports = num_ports_s;
        num_output_ports = num_ports_s;
        num_vcs = num_vcs_s;

        rr_vcs = new int[num_ports];
        for ( int i = 0; i < num_ports; i++ ) {
            rr_vcs[i] = 0;
        }

        rr_port = 0;
        service_rr_port = 0;
#if VERIFY_DECLOCKING
        rr_port_shadow = 0;
#endif
        vc_heads = nullptr;
    }

    bool setNetworkServiceInputs(int num_inputs, int num_outputs, int num_vcs_s) override
    {
        if ( num_inputs != num_outputs + 1 || num_outputs <= 0 ) return false;
        setPorts(num_inputs, num_vcs_s);
        num_output_ports = num_outputs;
        return true;
    }

#if VERIFY_DECLOCKING
    bool arbitrateNetworkService(XbarInput** inputs, PortInterface** outputs, int* input_busy,
        int* output_busy, int* progress_vc, bool clocking) override
#else
    bool arbitrateNetworkService(XbarInput** inputs, PortInterface** outputs, int* input_busy,
        int* output_busy, int* progress_vc) override
#endif
    {
        for ( int port = 0; port < num_ports; ++port ) progress_vc[port] = -1;

        const int synthetic_port = num_output_ports;
        internal_router_event** synthetic_heads = inputs[synthetic_port]->getVCHeads();
        bool synthetic_has_head = false;
        for ( int vc = 0; vc < num_vcs; ++vc ) {
            if ( synthetic_heads[vc] != nullptr ) {
                synthetic_has_head = true;
                break;
            }
        }

        auto arbitrate_port = [&](int port) -> bool {
            internal_router_event** heads = inputs[port]->getVCHeads();
            if ( input_busy[port] > 0 ) return true;
            bool had_head = false;

            for ( int vc = rr_vcs[port], checked = 0; checked < num_vcs;
                  vc = (vc + 1 == num_vcs ? 0 : vc + 1), ++checked ) {
                internal_router_event* event = heads[vc];
                if ( event == nullptr ) continue;
                had_head = true;
                const int next_port = event->getNextPort();
                const int next_vc   = event->getVC();
                if ( next_port < 0 || next_port >= num_output_ports || next_vc < 0 || next_vc >= num_vcs ) {
                    return false;
                }
                if ( output_busy[next_port] > 0 ||
                     !outputs[next_port]->spaceToSend(next_vc, event->getFlitCount()) ) {
                    continue;
                }
                progress_vc[port]     = vc;
                input_busy[port]      = event->getFlitCount();
                output_busy[next_port] = event->getFlitCount();
                break;
            }
            if ( had_head && progress_vc[port] == -1 ) progress_vc[port] = -2;
            rr_vcs[port] = (rr_vcs[port] + 1) % num_vcs;
            return true;
        };

        // Preserve the exact physical-port RR order whenever the synthetic
        // requester is empty.  While it is active, rotate first choice over
        // every individual physical and synthetic input with equal weight.
        if ( synthetic_has_head ) {
            for ( int port = service_rr_port, count = 0; count < num_ports;
                  port = (port + 1 == num_ports ? 0 : port + 1), ++count ) {
                if ( !arbitrate_port(port) ) return false;
            }
            service_rr_port = (service_rr_port + 1) % num_ports;
        }
        else {
            for ( int port = rr_port, count = 0; count < num_output_ports;
                  port = (port + 1 == num_output_ports ? 0 : port + 1), ++count ) {
                if ( !arbitrate_port(port) ) return false;
            }
            // Keep the per-VC cursor deterministic even while the requester
            // is empty, matching the treatment of an empty physical input.
            rr_vcs[synthetic_port] = (rr_vcs[synthetic_port] + 1) % num_vcs;
        }
        rr_port = (rr_port + 1) % num_output_ports;
#if VERIFY_DECLOCKING
        if ( clocking ) rr_port_shadow = rr_port;
#endif
        return true;
    }

    // Naming convention is from point of view of the xbar.  So,
    // in_port_busy is >0 if someone is writing to that xbar port and
    // out_port_busy is >0 if that xbar port being read.
    void arbitrate(
#if VERIFY_DECLOCKING
                   PortInterface** ports, int* in_port_busy, int* out_port_busy, int* progress_vc, bool clocking
#else
                   PortInterface** ports, int* in_port_busy, int* out_port_busy, int* progress_vc
#endif
                   ) override
    {
        // Run through each of the ports, giving first pick in a round robin fashion
        // for ( int port = rr_port, pcount = 0; pcount < num_ports; port = (port+1) % num_ports, pcount++ ) {
        for ( int port = rr_port, pcount = 0; pcount < num_ports; port = ((port != num_ports-1) ? port+1 : 0), pcount++ ) {

            vc_heads = ports[port]->getVCHeads();

            // Overwrite old data
            progress_vc[port] = -1;
            // if the output of this port is busy, nothing to do.
            if ( in_port_busy[port] > 0 ) {
                continue;
            }

            // See what we should progress for this port
            // for ( int vc = rr_vcs[port], vcount = 0; vcount < num_vcs; vc = (vc+1) % num_vcs, vcount++ ) {
            for ( int vc = rr_vcs[port], vcount = 0; vcount < num_vcs; vc = ((vc != num_vcs-1) ? (vc+1) : 0), vcount++ ) {

                // If there is no event, move to next VC
                internal_router_event* src_event = vc_heads[vc];
                if ( src_event == NULL ) continue;

                // Have an event, see if it can be progressed
                int next_port = src_event->getNextPort();

                // We can progress if the next port's input is not
                // busy and there are enough credits.
                if ( out_port_busy[next_port] > 0 ) continue;

                // Need to see if the VC has enough credits
                int next_vc = src_event->getVC();

                // See if there is enough space
                if ( !ports[next_port]->spaceToSend(next_vc, src_event->getFlitCount()) ) continue;

                // Tell the router what to move
                progress_vc[port] = vc;

                // Need to set the busy values
                in_port_busy[port] = src_event->getFlitCount();
                out_port_busy[next_port] = src_event->getFlitCount();
                break;  // Go to next port;
            }
            // Increemnt rr_vcs for next time
            rr_vcs[port] = (rr_vcs[port] + 1) % num_vcs;
        }
        rr_port = (rr_port + 1) % num_output_ports;

#if VERIFY_DECLOCKING
        if ( clocking ) {
            rr_port_shadow = rr_port;
        }
#endif

        return;
    }

    void reportSkippedCycles(Cycle_t cycles) override
    {
#if VERIFY_DECLOCKING
        rr_port_shadow = (rr_port_shadow + cycles) % num_output_ports;
        if ( rr_port_shadow != rr_port ) std::cout << "  PROBLEM:  rr_port = "
                         << rr_port << ", rr_port_shadow = " << rr_port_shadow <<
                         ", cycles = " << cycles << std::endl;
#else
        rr_port = (rr_port + cycles) % num_output_ports;
#endif
    }

    void dumpState(std::ostream& stream) override
    {
        stream << "Current round robin port: " << rr_port << std::endl;
        stream << "  Current round robin VC by port:" << std::endl;
        for ( int i = 0; i < num_ports; i++ ) {
            stream << i << ": " << rr_vcs[i] << std::endl;
        }
    }

};

}
}

#endif // COMPONENTS_HR_ROUTER_XBAR_ARB_RR_H
