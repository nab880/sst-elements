// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#ifndef SST_ELEMENTS_FIREFLY_COLLECTIVE_ADAPTER_H
#define SST_ELEMENTS_FIREFLY_COLLECTIVE_ADAPTER_H

#include "sst/elements/merlin/services/collective/collectiveEndpoint.h"
#include "sst/elements/merlin/services/collective/collectiveServiceData.h"

#include <cstdint>
#include <optional>

namespace SST::Firefly {

class VirtNic;

/**
 * One-vNIC proxy for the capability-validated static collective route.
 * It owns native completion state; the physical NIC owns wire construction.
 */
class FireflyCollectiveEndpoint final : public SST::Collective::CollectiveEndpoint
{
public:
    explicit FireflyCollectiveEndpoint(VirtNic& owner);

    bool publish(const SST::Collective::AcceptedParticipantHandle& participant);
    const SST::Collective::AcceptedParticipantHandle* participant(uint32_t local_slot) const;
    bool supportsCollective(
        const SST::Collective::CollectiveSignatureV1& signature) const override;

    bool bindParticipant(const SST::Collective::AcceptedParticipantHandle& participant,
        SST::Collective::CollectiveCompletionSink& completion,
        SST::Collective::CollectiveReadySink& ready) override;

    SST::Collective::CollectiveSubmitResult trySubmitCollective(
        SST::Collective::CollectivePending& pending) override;

    void requestCollectiveReady(
        const SST::Collective::AcceptedParticipantHandle& participant,
        const SST::Collective::CollectiveSignatureV1& signature) override;

    void submitAccepted(uint64_t invocation_id);
    void receiveResult(SST::Collective::StaticCollectiveResult result);
    bool notifyReadyIfPossible();

private:
    bool sameParticipant(const SST::Collective::AcceptedParticipantHandle& participant) const;

    VirtNic& owner_;
    SST::Collective::AcceptedParticipantHandle accepted_;
    bool published_ = false;

    SST::Collective::CollectiveCompletionSink* completion_ = nullptr;
    SST::Collective::CollectiveReadySink* ready_ = nullptr;
    std::optional<SST::Collective::CollectiveCompletionToken> token_;
    SST::Collective::MutableBufferView result_;
    SST::Collective::CollectiveSignatureV1 active_signature_;
    SST::Collective::CollectiveSignatureV1 ready_signature_;
    uint64_t active_invocation_ = 0;
    uint64_t awaiting_ack_invocation_ = 0;
    uint64_t completed_invocation_ = 0;
    bool ready_armed_ = false;
};

} // namespace SST::Firefly

#endif // SST_ELEMENTS_FIREFLY_COLLECTIVE_ADAPTER_H
