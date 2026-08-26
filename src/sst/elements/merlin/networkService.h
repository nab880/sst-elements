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
#include <memory>

namespace SST::Merlin {

class internal_router_event;

using NetworkServiceID = SST::Interfaces::SimpleNetwork::NetworkServiceID;

enum class NetworkServiceDisposition : uint8_t { Pass = 1, Accept = 2, Busy = 3, Reject = 4 };

inline constexpr bool isValid(NetworkServiceDisposition disposition)
{
    return disposition >= NetworkServiceDisposition::Pass && disposition <= NetworkServiceDisposition::Reject;
}

/** Stable identity of one queued VC head during a prepare/apply transaction. */
struct NetworkServiceHeadIdentity
{
    const internal_router_event* event      = nullptr;
    uint64_t                     generation = 0;

    constexpr bool valid() const { return event != nullptr && generation != 0; }
};

/** Non-owning view passed to a service processor during prepare(). */
struct NetworkServiceIngress
{
    int                        input_port = -1;
    int                        input_vc   = -1;
    NetworkServiceHeadIdentity head;
    const internal_router_event* event = nullptr;
};

/**
 * Opaque, rollback-capable reservation returned only with Accept.  The
 * router never interprets service state; it invokes exactly one terminal
 * method and then destroys the reservation.
 */
class NetworkServiceReservation
{
public:
    virtual ~NetworkServiceReservation() = default;
    virtual void commit(std::unique_ptr<internal_router_event> event) noexcept = 0;
    virtual void rollback() noexcept = 0;
};

struct NetworkServicePrepared
{
    NetworkServiceDisposition                    disposition = NetworkServiceDisposition::Reject;
    std::unique_ptr<NetworkServiceReservation>   reservation;
    uint64_t                                     opaque_diagnostic = 0;

    NetworkServicePrepared() = default;
    NetworkServicePrepared(NetworkServiceDisposition disposition,
        std::unique_ptr<NetworkServiceReservation> reservation = {}, uint64_t opaque_diagnostic = 0) :
        disposition(disposition),
        reservation(std::move(reservation)),
        opaque_diagnostic(opaque_diagnostic)
    {}

    NetworkServicePrepared(const NetworkServicePrepared&)            = delete;
    NetworkServicePrepared& operator=(const NetworkServicePrepared&) = delete;
    NetworkServicePrepared(NetworkServicePrepared&&) noexcept        = default;
    NetworkServicePrepared& operator=(NetworkServicePrepared&&) noexcept = default;
};

/** Minimal queue seam used to make exact-head dequeue independently testable. */
class NetworkServiceIngressQueue
{
public:
    virtual ~NetworkServiceIngressQueue() = default;
    virtual NetworkServiceHeadIdentity inspectNetworkServiceHead(int vc) const = 0;
    virtual internal_router_event* recvNetworkServiceExpected(
        int vc, const NetworkServiceHeadIdentity& expected) = 0;
};

enum class NetworkServiceApplyResult : uint8_t {
    Passed = 1,
    Accepted,
    Busy,
    Rejected,
    HeadChanged,
    InvalidPrepared
};

NetworkServiceApplyResult applyNetworkServicePrepared(NetworkServiceIngressQueue& queue, int vc,
    const NetworkServiceHeadIdentity& expected, NetworkServicePrepared&& prepared,
    uint64_t& opaque_diagnostic) noexcept;

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
    virtual NetworkServicePrepared prepare(const NetworkServiceIngress& ingress) = 0;
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
    NetworkServicePrepared prepare(const NetworkServiceIngress& ingress) override;
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
