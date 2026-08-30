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
#include <exception>
#include <memory>

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

enum class NetworkServiceDisposition : uint8_t { Pass = 1, Accept = 2, Busy = 3, Reject = 4 };

inline constexpr bool isValid(NetworkServiceDisposition disposition)
{
    return disposition >= NetworkServiceDisposition::Pass && disposition <= NetworkServiceDisposition::Reject;
}

/** Non-owning view passed to a service processor during inspect(). */
struct NetworkServiceIngress
{
    int input_port = -1;
    int input_vc   = -1;
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
    int input_vc   = -1;
    std::unique_ptr<internal_router_event> event;
};

/** Minimal queue seam used to make exact-head dequeue independently testable. */
class NetworkServiceIngressQueue
{
public:
    virtual ~NetworkServiceIngressQueue() = default;
    virtual const internal_router_event* inspectNetworkServiceHead(int vc) const = 0;
    /** Remove and return exactly expected, or return null without modifying the queue. */
    virtual internal_router_event* recvNetworkServiceExpected(
        int vc, const internal_router_event* expected) = 0;
};

enum class NetworkServiceTakeResult : uint8_t { Taken = 1, HeadChanged, InvalidExpected };

NetworkServiceTakeResult takeNetworkServiceIngressExpected(NetworkServiceIngressQueue& queue,
    int input_port, int input_vc, const internal_router_event* expected,
    NetworkServiceOwnedIngress& ingress) noexcept;

/** Move-only fresh packet offered to the router's bounded synthetic requester. */
struct NetworkServiceSyntheticPacket
{
    std::unique_ptr<SST::Interfaces::SimpleNetwork::Request> request;
    SST::Interfaces::SimpleNetwork::nid_t trusted_src = -1;
    int      route_vn       = -1;
    int      output_port    = -1;
    int      output_vc      = -1;
    int      size_in_flits  = 0;

    bool valid(NetworkServiceID service_id) const;
};

class NetworkServiceHost
{
public:
    virtual ~NetworkServiceHost() = default;

    /** Consumes packet.request only on success. */
    virtual bool tryEnqueueNetworkServiceOutput(
        NetworkServiceID service_id, NetworkServiceSyntheticPacket& packet) = 0;

    /** Required after asynchronous work becomes ready while the router may be declocked. */
    virtual void wakeNetworkServiceProcessor() = 0;

    /** Optional router-side discovery and head-accounting hooks. */
    virtual NetworkServiceID getNetworkServiceID() const
    {
        return SST::Interfaces::SimpleNetwork::NETWORK_SERVICE_NONE;
    }
    virtual NetworkServiceRequestContract getNetworkServiceRequestContract() const { return {}; }
    virtual void networkServiceHeadAppeared() {}
    virtual void networkServiceHeadRemoved() {}
};

/** Service-neutral Merlin processor API. */
class NetworkServiceProcessor : public SST::SubComponent
{
public:
    SST_ELI_REGISTER_SUBCOMPONENT_API(SST::Merlin::NetworkServiceProcessor, SST::Merlin::NetworkServiceHost*)

    explicit NetworkServiceProcessor(ComponentId_t id) : SST::SubComponent(id) {}
    NetworkServiceProcessor() : SST::SubComponent() {}
    ~NetworkServiceProcessor() override = default;

    virtual NetworkServiceID getServiceID() const = 0;
    virtual NetworkServiceRequestContract getRequestContract() const
    {
        return { getServiceID(), 0, 0, 0 };
    }
    /** Inspect only; implementations must not mutate processor or router state. */
    virtual NetworkServiceDecision inspect(const NetworkServiceIngress& ingress) const = 0;
    /** Terminal ownership transfer after the router dequeues the exact accepted head. */
    virtual void consume(NetworkServiceOwnedIngress ingress) noexcept = 0;
    virtual bool hasScheduledWork() const = 0;

    void serialize_order(SST::Core::Serialization::serializer& ser) override
    {
        SST::SubComponent::serialize_order(ser);
    }

    ImplementVirtualSerializable(SST::Merlin::NetworkServiceProcessor)
};

/** Generic processor used to validate transparent tagged carriage. */
class NetworkServicePassProcessor final : public NetworkServiceProcessor
{
public:
    SST_ELI_REGISTER_SUBCOMPONENT(NetworkServicePassProcessor, "merlin", "network_service_pass",
        SST_ELI_ELEMENT_VERSION(1, 0, 0), "Pass tagged packets through ordinary Merlin routing",
        SST::Merlin::NetworkServiceProcessor)

    SST_ELI_DOCUMENT_PARAMS(
        { "service_id", "Nonzero 16-bit network-service ID to pass", "" }
    )

    NetworkServicePassProcessor(ComponentId_t id, Params& params, NetworkServiceHost* host);
    NetworkServicePassProcessor() = default;

    NetworkServiceID getServiceID() const override { return service_id_; }
    NetworkServiceDecision inspect(const NetworkServiceIngress& ingress) const override;
    void consume(NetworkServiceOwnedIngress) noexcept override { std::terminate(); }
    bool hasScheduledWork() const override { return false; }

    void serialize_order(SST::Core::Serialization::serializer& ser) override
    {
        NetworkServiceProcessor::serialize_order(ser);
        SST_SER(service_id_);
    }

private:
    NetworkServiceID service_id_ = SST::Interfaces::SimpleNetwork::NETWORK_SERVICE_NONE;

    ImplementSerializable(SST::Merlin::NetworkServicePassProcessor)
};

} // namespace SST::Merlin

#endif // SST_ELEMENTS_MERLIN_NETWORK_SERVICE_H
