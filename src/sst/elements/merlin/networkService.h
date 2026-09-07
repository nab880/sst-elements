// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#ifndef SST_ELEMENTS_MERLIN_NETWORK_SERVICE_H
#define SST_ELEMENTS_MERLIN_NETWORK_SERVICE_H

#include <sst/core/interfaces/simpleNetwork.h>
#include <sst/core/subcomponent.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace SST::Merlin {

class internal_router_event;

using NetworkServiceID = SST::Interfaces::SimpleNetwork::NetworkServiceID;
using NetworkServiceDataToken = SST::Interfaces::SimpleNetwork::NetworkServiceDataToken;
using NetworkServiceVersion = SST::Interfaces::SimpleNetwork::NetworkServiceVersion;

/** Request shape a processor can safely consume. */
struct NetworkServiceRequestContract
{
    NetworkServiceID service_id = SST::Interfaces::SimpleNetwork::NETWORK_SERVICE_NONE;
    NetworkServiceDataToken data_token = 0;
    NetworkServiceVersion min_schema_version = 0;
    NetworkServiceVersion max_schema_version = 0;

    constexpr bool valid() const
    {
        if ( service_id == SST::Interfaces::SimpleNetwork::NETWORK_SERVICE_NONE ) {
            return data_token == 0 && min_schema_version == 0 && max_schema_version == 0;
        }
        return data_token == 0 ? min_schema_version == 0 && max_schema_version == 0 :
                                 min_schema_version <= max_schema_version;
    }

    bool accepts(const SST::Interfaces::SimpleNetwork::Request& request) const
    {
        return valid() && request.getServiceID() == service_id && request.inspectServiceData() != nullptr &&
               (data_token == 0 || request.serviceDataMatches(
                    service_id, data_token, min_schema_version, max_schema_version));
    }

    void serialize_order(SST::Core::Serialization::serializer& ser)
    {
        SST_SER(service_id);
        SST_SER(data_token);
        SST_SER(min_schema_version);
        SST_SER(max_schema_version);
    }
};

/**
 * Disposition for one head on a VN the processor owns.  Accept transfers
 * the dequeued head to consume() and returns its ingress credits normally.
 * Busy leaves the head where it is; only that VC waits.  Reject is terminal:
 * the router fails the simulation and reports the opaque diagnostic.
 */
enum class NetworkServiceDisposition : uint8_t { Accept = 1, Busy = 2, Reject = 3 };

inline constexpr bool isValid(NetworkServiceDisposition disposition)
{
    return disposition >= NetworkServiceDisposition::Accept && disposition <= NetworkServiceDisposition::Reject;
}

/** Non-owning view passed to a service processor during inspect(). */
struct NetworkServiceIngress
{
    int input_port = -1;
    int input_vn   = -1;
    const internal_router_event* event = nullptr;
};

/** Read-only disposition for the current head. */
struct NetworkServiceDecision
{
    NetworkServiceDisposition disposition = NetworkServiceDisposition::Reject;
    uint64_t                  opaque_diagnostic = 0;
};

/** Exact dequeued head whose ownership is transferred after Accept. */
struct NetworkServiceOwnedIngress
{
    int input_port = -1;
    int input_vn   = -1;
    std::unique_ptr<internal_router_event> event;
};

/**
 * Move-only fresh packet offered to the router's bounded synthetic requester.
 * The router chooses the VC: service egress enters on the first VC of
 * route_vn, exactly like a fresh injection from an endpoint.
 */
struct NetworkServiceSyntheticPacket
{
    std::unique_ptr<SST::Interfaces::SimpleNetwork::Request> request;
    SST::Interfaces::SimpleNetwork::nid_t trusted_src = -1;
    int      route_vn       = -1;
    int      output_port    = -1;

    bool valid(NetworkServiceID service_id) const;
    void serialize_order(SST::Core::Serialization::serializer& ser);
};

/** Immutable transport requirements used to reject bad static routes before timed execution. */
struct NetworkServiceOutputSpec
{
    int    route_vn      = -1;
    int    output_port   = -1;
    size_t size_in_bits  = 0;

    constexpr bool valid() const
    {
        return route_vn >= 0 && output_port >= 0 && size_in_bits > 0;
    }
};

class NetworkServiceHost
{
public:
    virtual ~NetworkServiceHost() = default;

    /** Pure validation: no queue-capacity check and no ownership transfer. */
    virtual bool supportsNetworkServiceOutput(const NetworkServiceOutputSpec& spec) const = 0;

    /** Advisory readiness check for lazy packet construction; transfers no ownership. */
    virtual bool canEnqueueNetworkServiceOutput(const NetworkServiceOutputSpec& spec) const
    {
        return supportsNetworkServiceOutput(spec);
    }

    /** Consumes packet.request only on success. */
    virtual bool tryEnqueueNetworkServiceOutput(
        NetworkServiceID service_id, NetworkServiceSyntheticPacket& packet) = 0;

    /** Required after asynchronous work becomes ready while the router may be declocked. */
    virtual void wakeNetworkServiceProcessor() = 0;

    /** Optional router-side discovery hooks. */
    virtual NetworkServiceID getNetworkServiceID() const
    {
        return SST::Interfaces::SimpleNetwork::NETWORK_SERVICE_NONE;
    }
    virtual NetworkServiceRequestContract getNetworkServiceRequestContract() const { return {}; }
};

/**
 * Service-neutral Merlin processor API.
 *
 * A processor owns a fixed set of router VNs.  The router maps each owned VN
 * to the VCs the topology assigns it; the crossbar never arbitrates a head
 * on one of those VCs.  Instead the router offers each such head to
 * inspect() every cycle and dequeues it into consume() on Accept.  Traffic
 * on VNs the processor does not own is ordinary Merlin traffic, whether or
 * not it carries a service tag.
 */
class NetworkServiceProcessor : public SST::SubComponent
{
public:
    SST_ELI_REGISTER_SUBCOMPONENT_API(SST::Merlin::NetworkServiceProcessor, SST::Merlin::NetworkServiceHost*)

    NetworkServiceProcessor(ComponentId_t id, NetworkServiceHost* host) : SST::SubComponent(id), host_(host) {}
    NetworkServiceProcessor() : SST::SubComponent() {}
    ~NetworkServiceProcessor() override = default;

    NetworkServiceHost* host() const { return host_; }
    /** Re-attaches the owning router after checkpoint restore. */
    void bindHost(NetworkServiceHost* host) { host_ = host; }

    virtual NetworkServiceID getServiceID() const = 0;
    virtual NetworkServiceRequestContract getRequestContract() const
    {
        return { getServiceID(), 0, 0, 0 };
    }
    /** Router VNs this processor owns for the life of the simulation; empty owns nothing. */
    virtual std::vector<int> ownedVNs() const = 0;
    /** Re-check transport facts learned during init before timed execution. */
    virtual bool validateInstalledTransport() const { return true; }
    /** Inspect only; implementations must not mutate processor or router state. */
    virtual NetworkServiceDecision inspect(const NetworkServiceIngress& ingress) const = 0;
    /** Terminal ownership transfer after the router dequeues the accepted head. */
    virtual void consume(NetworkServiceOwnedIngress ingress) noexcept = 0;
    virtual bool hasScheduledWork() const = 0;
    /** Driven by the router clock while hasScheduledWork(); returns true once nothing remains. */
    virtual bool progress() { return true; }

    void serialize_order(SST::Core::Serialization::serializer& ser) override
    {
        SST::SubComponent::serialize_order(ser);
    }

    ImplementVirtualSerializable(SST::Merlin::NetworkServiceProcessor)

private:
    NetworkServiceHost* host_ = nullptr; // non-owning; rebound by the router after restore
};

} // namespace SST::Merlin

#endif // SST_ELEMENTS_MERLIN_NETWORK_SERVICE_H
