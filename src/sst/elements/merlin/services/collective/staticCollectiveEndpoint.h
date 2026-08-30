// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#ifndef SST_ELEMENTS_STATIC_COLLECTIVE_ENDPOINT_H
#define SST_ELEMENTS_STATIC_COLLECTIVE_ENDPOINT_H

#include "collectiveEndpoint.h"
#include "collectiveServiceData.h"

#include <cstdint>

namespace SST::Collective {

/** Shared ownership state for the fixed one-participant static collective profile. */
class StaticCollectiveEndpointBase : public CollectiveEndpoint
{
public:
    const AcceptedParticipantHandle* participant(uint32_t local_slot) const;
    bool supportsCollective(const CollectiveSignatureV1& signature) const final;

    bool bindParticipant(const AcceptedParticipantHandle& participant,
        CollectiveCompletionSink& completion, CollectiveReadySink& ready) final;
    CollectiveSubmitResult trySubmitCollective(CollectivePending& pending) final;
    void requestCollectiveReady(const AcceptedParticipantHandle& participant,
        const CollectiveSignatureV1& signature) final;

    bool notifyReadyIfPossible();
    bool quiescent() const;

protected:
    bool installParticipant(const AcceptedParticipantHandle& participant);
    const AcceptedParticipantHandle& acceptedParticipant() const { return accepted_; }
    uint64_t activeInvocation() const { return active_invocation_; }

    bool completeSuccess(const StaticCollectiveResult& result);

    virtual bool transportReady(const CollectiveSignatureV1& signature) const = 0;
    /**
     * Commits an accepted contribution or terminates.  The hook cannot
     * decline or roll back ownership, and transport completion must arrive
     * after trySubmitCollective() returns to its native caller.
     */
    virtual void commitContribution(const AcceptedParticipantHandle& participant,
        StaticCollectiveContribution&& contribution) noexcept = 0;

private:
    bool sameParticipant(const AcceptedParticipantHandle& participant) const;

    AcceptedParticipantHandle accepted_;
    CollectiveCompletionSink* completion_ = nullptr;
    CollectiveReadySink*      ready_sink_ = nullptr;
    CollectiveCompletionToken token_;
    MutableBufferView         result_;
    CollectiveSignatureV1     active_signature_;
    CollectiveSignatureV1     ready_signature_;
    uint64_t                  active_invocation_ = 0;
    uint64_t                  retired_invocation_ = 0;
    bool                      ready_armed_ = false;
};

/** Checks the fixed sidecar, schema, feature, and atomic-packet transport contract. */
bool supportsStaticCollectiveTransport(const SimpleNetwork& network, int reduce_vn, int result_vn);

} // namespace SST::Collective

#endif // SST_ELEMENTS_STATIC_COLLECTIVE_ENDPOINT_H
