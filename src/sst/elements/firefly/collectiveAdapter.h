// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#ifndef SST_ELEMENTS_FIREFLY_COLLECTIVE_ADAPTER_H
#define SST_ELEMENTS_FIREFLY_COLLECTIVE_ADAPTER_H

#include "sst/elements/merlin/services/collective/staticCollectiveEndpoint.h"

#include <cstdint>

namespace SST::Firefly {

class VirtNic;

/**
 * One-vNIC proxy for the capability-validated static collective route.
 * The shared base owns native completion state; the physical NIC owns wire construction.
 */
class FireflyCollectiveEndpoint final : public SST::Collective::StaticCollectiveEndpointBase
{
public:
    explicit FireflyCollectiveEndpoint(VirtNic& owner);

    bool publish(const SST::Collective::AcceptedParticipantHandle& participant)
    {
        return installParticipant(participant);
    }

    void submitAccepted(uint64_t invocation_id);
    void receiveResult(const SST::Collective::StaticCollectiveResult& result);

private:
    bool transportReady(const SST::Collective::CollectiveSignatureV1& signature) const override;
    void commitContribution(const SST::Collective::AcceptedParticipantHandle& participant,
        SST::Collective::StaticCollectiveContribution&& contribution) noexcept override;

    VirtNic& owner_;
    uint64_t awaiting_ack_invocation_ = 0;
};

} // namespace SST::Firefly

#endif // SST_ELEMENTS_FIREFLY_COLLECTIVE_ADAPTER_H
