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

namespace SST::Merlin {

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
 * A processor names one network service and the request shape it accepts,
 * and may emit synthetic packets through its host's bounded requester, which
 * the crossbar arbitrates like a fresh injection.  Tagged packets travel as
 * ordinary Merlin traffic; a later layer adds processor-owned ingress.
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
    /**
     * False only for a processor that never calls tryEnqueueNetworkServiceOutput.
     * A dormant processor only advertises its service, so an arbiter without
     * synthetic-input support (merlin.xbar_arb_rr) can host it.  The router
     * refuses synthetic output from a processor that reports false.
     */
    virtual bool emitsSyntheticPackets() const { return true; }
    /** Re-check transport facts learned during init before timed execution. */
    virtual bool validateInstalledTransport() const { return true; }
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
