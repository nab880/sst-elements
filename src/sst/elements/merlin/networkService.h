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

#include <cstdint>

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

class NetworkServiceHost
{
public:
    virtual ~NetworkServiceHost() = default;

    /** Router-side discovery hooks used to advertise one service to attached endpoints. */
    virtual NetworkServiceID getNetworkServiceID() const
    {
        return SST::Interfaces::SimpleNetwork::NETWORK_SERVICE_NONE;
    }
    virtual NetworkServiceRequestContract getNetworkServiceRequestContract() const { return {}; }
};

/**
 * Service-neutral Merlin processor API.
 *
 * A processor names one network service and the request shape it accepts.
 * The router advertises both to every attached endpoint during init, and
 * endpoints negotiate against them before sending tagged requests.  Tagged
 * packets travel as ordinary Merlin traffic; later layers add synthetic
 * egress and processor-owned ingress.
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
