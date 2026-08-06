#ifndef COMPONENTS_HR_ROUTER_COLLECTIVE_ACCEL_H
#define COMPONENTS_HR_ROUTER_COLLECTIVE_ACCEL_H

#include <sst/core/component.h>
#include <sst/core/event.h>
#include <sst/core/link.h>
#include <sst/core/timeConverter.h>

#include <vector>

#include "sst/elements/merlin/router.h"

// RING_TYPE == 0 ->            crossbar
// RING_TYPE == 1 -> unidirectional ring
// RING_TYPE == 2 ->  bidirectional ring
#define RING_TYPE 1

#define CYCLES_PER_FLIT 1

using namespace SST;

namespace SST {
namespace Merlin {

class incRingEvent : public Event {
public:
    int job_id;
    int data;
    int flit_count;
    int next_port;
    int root_port;
    bool compute;
    incRingEvent() {}
    incRingEvent(int job_id, int flit_count, int next_port, int root_port) : job_id(job_id), flit_count(flit_count), data(0), next_port(next_port), root_port(root_port), compute(true) {}
    incRingEvent(int job_id, int flit_count, int next_port, int root_port, int data) : job_id(job_id), flit_count(flit_count), data(data), next_port(next_port), root_port(root_port), compute(false) {}

    void serialize_order(SST::Core::Serialization::serializer &ser) override {
        Event::serialize_order(ser);
        SST_SER(job_id);
        SST_SER(flit_count);
        SST_SER(data);
        SST_SER(next_port);
        SST_SER(root_port);
        SST_SER(compute);
    }

private:
    ImplementSerializable(SST::Merlin::incRingEvent);
};

class collective_accel : public Accelerator {

public:

    SST_ELI_REGISTER_SUBCOMPONENT(
        collective_accel,
        "merlin",
        "collective_accel",
        SST_ELI_ELEMENT_VERSION(1,0,0),
        "Collective acceleration unit for hr_router to enable in-network compute",
        SST::Merlin::Accelerator
    )

    SST_ELI_DOCUMENT_PORTS(
        {"lport", "Link to the collective acceleration unit to the left of this subcomponent", {}},
        {"rport", "Link to the collective acceleration unit to the right of this subcomponent", {}}
    )

private:

    Router* parent;
    int port_number;

    Link* llink;
    Link* rlink;

    std::unordered_map<int, internal_router_event*> ireQ;
    std::queue<incRingEvent*> ringQ;
    std::queue<incRingEvent*> xbarQ;
    std::queue<std::pair<int, internal_router_event*>> sendQ;

    Link* compute_link;
    Link* xbar_link;

    int in_accel_busy;
    int out_accel_busy;

public:

    collective_accel(ComponentId_t cid, Params& params, Router* parent, int port_number) : Accelerator(cid), parent(parent), port_number(port_number), in_accel_busy(0), out_accel_busy(0) {
        registerClock("1GHz", new Clock::Handler<collective_accel,&collective_accel::clock_handler>(this), false);

#if RING_TYPE > 0
        llink = configureLink("lport", new Event::Handler<collective_accel, &collective_accel::lhandle>(this));
        if (!llink) merlin_abort.fatal(CALL_INFO_LONG, 1, "collective_accel requires link to lport\n");
        rlink = configureLink("rport", new Event::Handler<collective_accel, &collective_accel::rhandle>(this));
        if (!rlink) merlin_abort.fatal(CALL_INFO_LONG, 1, "collective_accel requires link to rport\n");
#endif

        compute_link = configureSelfLink("compute_link", new Event::Handler<collective_accel, &collective_accel::handle_compute>(this));
        xbar_link = configureSelfLink("xbar_link", "10ns", new Event::Handler<collective_accel, &collective_accel::handle_xbar>(this));
    }

    ~collective_accel() {}

    int getInAccelBusy() { return in_accel_busy; }

private:

    void sendRing(incRingEvent* ring_ev) {
#if RING_TYPE == 0
        if (out_accel_busy <= 0 && parent->getInAccelBusy(ring_ev->next_port) <= 0) {
            out_accel_busy += CYCLES_PER_FLIT*ring_ev->flit_count;
            xbar_link->send(CYCLES_PER_FLIT*ring_ev->flit_count, ring_ev);
	} else {
	    xbarQ.push(ring_ev);
	}
#elif RING_TYPE == 1
        if (out_accel_busy <= 0 && parent->getInAccelBusy((port_number+1)%parent->getNumPorts()) <= 0) {
            out_accel_busy += CYCLES_PER_FLIT*ring_ev->flit_count;
            rlink->send(CYCLES_PER_FLIT*ring_ev->flit_count, ring_ev);
	} else {
	    xbarQ.push(ring_ev);
	}
#else
        int dist1 = std::abs(ring_ev->next_port-port_number);
        int dist2 = parent->getNumPorts()-dist1;

        if (ring_ev->next_port < port_number) {
            if (dist1 < dist2) {
                if (out_accel_busy <= 0 && parent->getInAccelBusy(((port_number-1)%parent->getNumPorts()+parent->getNumPorts())%parent->getNumPorts()) <= 0) {
                    out_accel_busy += CYCLES_PER_FLIT*ring_ev->flit_count;
                    llink->send(CYCLES_PER_FLIT*ring_ev->flit_count, ring_ev);
	        } else {
	            xbarQ.push(ring_ev);
	        }
            } else {
                if (out_accel_busy <= 0 && parent->getInAccelBusy((port_number+1)%parent->getNumPorts()) <= 0) {
                    out_accel_busy += CYCLES_PER_FLIT*ring_ev->flit_count;
                    rlink->send(CYCLES_PER_FLIT*ring_ev->flit_count, ring_ev);
	        } else {
	            xbarQ.push(ring_ev);
	        }
            }
        } else {
            if (dist1 > dist2) {
                if (out_accel_busy <= 0 && parent->getInAccelBusy(((port_number-1)%parent->getNumPorts()+parent->getNumPorts())%parent->getNumPorts()) <= 0) {
                    out_accel_busy += CYCLES_PER_FLIT*ring_ev->flit_count;
                    llink->send(CYCLES_PER_FLIT*ring_ev->flit_count, ring_ev);
	        } else {
	            xbarQ.push(ring_ev);
	        }
            } else {
                if (out_accel_busy <= 0 && parent->getInAccelBusy((port_number+1)%parent->getNumPorts()) <= 0) {
                    out_accel_busy += CYCLES_PER_FLIT*ring_ev->flit_count;
                    rlink->send(CYCLES_PER_FLIT*ring_ev->flit_count, ring_ev);
	        } else {
	            xbarQ.push(ring_ev);
	        }
            }
        }
#endif
    }

    void lhandle(Event* ev) {
        incRingEvent* ring_ev = static_cast<incRingEvent*>(ev);

        if (ring_ev == NULL) {
            merlin_abort.fatal(CALL_INFO_LONG, 1, "collective_accel received a non-incRingEvent on lport\n");
        }

        in_accel_busy += CYCLES_PER_FLIT*ring_ev->flit_count;

        if (port_number == ring_ev->next_port) {
            compute_link->send(ring_ev);
        } else {
            sendRing(ring_ev);
        }
    }

    void rhandle(Event* ev) {
        incRingEvent* ring_ev = static_cast<incRingEvent*>(ev);

        if (ring_ev == NULL) {
            merlin_abort.fatal(CALL_INFO_LONG, 1, "collective_accel received a non-incRingEvent on lport\n");
        }

        in_accel_busy += CYCLES_PER_FLIT*ring_ev->flit_count;

        if (port_number == ring_ev->next_port) {
            compute_link->send(ring_ev);
        } else {
            sendRing(ring_ev);
        }
    }

    void handle_compute(Event* ev) {
        incRingEvent* ring_ev = static_cast<incRingEvent*>(ev);

        if (ring_ev == NULL) {
            merlin_abort.fatal(CALL_INFO_LONG, 1, "collective_accel received a non-incRingEvent on compute_link\n");
        }

#if RING_TYPE == 0
        in_accel_busy += CYCLES_PER_FLIT*ring_ev->flit_count;
#endif

        ringQ.push(ring_ev);
    }

    void handle_xbar(Event* ev) {
        incRingEvent* ring_ev = static_cast<incRingEvent*>(ev);

        if (ring_ev == NULL) {
            merlin_abort.fatal(CALL_INFO_LONG, 1, "collective_accel received a non-incRingEvent on compute_link\n");
        }

        parent->xbarINC(ring_ev->next_port, ring_ev);
    }

    bool clock_handler(Cycle_t cycle) {
        in_accel_busy = std::max(0, in_accel_busy-1);
        out_accel_busy = std::max(0, out_accel_busy-1);

        if (sendQ.size() > 0) {
            int send_port = sendQ.front().first;
            internal_router_event* send_ire = sendQ.front().second;
            sendQ.pop();

            if (parent->sendINC(send_port, send_ire)) {
                // do nothing
            } else {
                sendQ.push(std::pair<int, internal_router_event*>(send_port, send_ire));
            }
        } else if (xbarQ.size() > 0) {
            incRingEvent* xbar_ev = xbarQ.front();
            xbarQ.pop();

            sendRing(xbar_ev);
        } else if (ringQ.size() > 0) {
            incRingEvent* ring_ev = ringQ.front();
            ringQ.pop();

            auto find_ire = ireQ.find(ring_ev->job_id);
            if (find_ire != ireQ.end()) {
                internal_router_event* ire = find_ire->second;
                incEvent* inc_ev = dynamic_cast<incEvent*>(ire->inspectRequest()->inspectPayload());
                if (ring_ev->compute) {
                    ring_ev->data += inc_ev->data;
                    if (port_number == ring_ev->root_port) {
                        if (parent->getLevel() == inc_ev->up_ports.size()) {
                            ring_ev->compute = false;
                            ring_ev->next_port = inc_ev->next_ports[parent->getLevel()];

                            sendRing(ring_ev);
                        } else {
                            inc_ev->data = ring_ev->data;
                            sendQ.push(std::pair<int, internal_router_event*>(inc_ev->up_ports[parent->getLevel()], ire));
                            ireQ.erase(find_ire);
                        }
                    } else {
                        ring_ev->next_port = inc_ev->next_ports[parent->getLevel()];

                        sendRing(ring_ev);
                    }
                } else {
                    inc_ev->data = ring_ev->data;
                    sendQ.push(std::pair<int, internal_router_event*>(port_number, ire));

                    if (port_number == ring_ev->root_port) {
                        delete ring_ev;
                    } else {
                        ring_ev->next_port = inc_ev->next_ports[parent->getLevel()];

                        sendRing(ring_ev);
                    }

                    ireQ.erase(find_ire);
                }
            } else {
                ringQ.push(ring_ev);
            }
        }

        return false;
    }

    void startINC(internal_router_event* ire, bool compute)
    {
        incEvent* inc_ev = dynamic_cast<incEvent*>(ire->inspectRequest()->inspectPayload());
        if (inc_ev) {
            ireQ[inc_ev->job_id] = ire;
            if (port_number == inc_ev->root_ports[parent->getLevel()]) {
                incRingEvent* ring_ev;
                if (compute) {
                    ring_ev = new incRingEvent(inc_ev->job_id, ire->getFlitCount(), inc_ev->next_ports[parent->getLevel()], inc_ev->root_ports[parent->getLevel()]);
                } else {
                    ring_ev = new incRingEvent(inc_ev->job_id, ire->getFlitCount(), inc_ev->next_ports[parent->getLevel()], inc_ev->root_ports[parent->getLevel()], inc_ev->data);
                }

                sendRing(ring_ev);
            }
        }
    }
};

}
}

#endif // COMPONENTS_HR_ROUTER_COLLECTIVE_ACCEL_H
