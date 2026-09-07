// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include <sst_config.h>

#include "collectiveAdapter.h"
#include "nic.h"
#include "virtNic.h"

#include <memory>

namespace SST::Firefly {
using namespace SST::Collective;

FireflyCollectiveEndpoint::FireflyCollectiveEndpoint(VirtNic& owner) : owner_(owner) {}

CollectiveSubmitResult FireflyCollectiveEndpoint::submitWithMemory(
    const CollectiveSubmission& submission, uint64_t source_address, uint64_t result_address)
{
    source_address_ = source_address;
    result_address_ = result_address;
    const auto result = trySubmitCollective(submission);
    source_address_ = result_address_ = 0;
    return result;
}

bool FireflyCollectiveEndpoint::transportReady(const CollectiveSignatureV1&) const
{
    return owner_.collectiveCommandSlotAvailable();
}

void FireflyCollectiveEndpoint::commitContribution(StaticCollectiveContribution&& contribution) noexcept
{
    const uint64_t invocation_id = contribution.invocation_id;
    auto event = std::make_unique<NicCollectiveSubmitCmdEvent>(
        std::move(contribution), source_address_, result_address_);
    awaiting_ack_invocation_ = invocation_id;
    owner_.sendCollectiveCommand(event.release());
}

void FireflyCollectiveEndpoint::submitAccepted(uint64_t invocation_id)
{
    if ( awaiting_ack_invocation_ == 0 || invocation_id != awaiting_ack_invocation_ ) {
        owner_.collectiveFatal("Mismatched or duplicate collective submit acceptance");
    }
    awaiting_ack_invocation_ = 0;
}

void FireflyCollectiveEndpoint::receiveResult(const StaticCollectiveResult& result)
{
    if ( !completeSuccess(result) ) {
        owner_.collectiveFatal("Mismatched or duplicate collective result");
    }
}

} // namespace SST::Firefly
