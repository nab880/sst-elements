// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.

#include <sst_config.h>

#include "sst/elements/firefly/virtNic.h"

#include <sst/core/component.h>
#include <sst/core/link.h>

namespace SST::Firefly {

/** Real vNIC/NIC integration: in-place snapshots survive asynchronous DMA. */
class CollectiveHostTimingTest final : public SST::Component,
                                      public SST::Collective::CollectiveCompletionSink,
                                      public SST::Collective::CollectiveReadySink
{
public:
    SST_ELI_REGISTER_COMPONENT(CollectiveHostTimingTest, "firefly", "collective_host_timing_test",
        SST_ELI_ELEMENT_VERSION(1, 0, 0), "Exercise collective host DMA timing and in-place ownership",
        COMPONENT_CATEGORY_PROCESSOR)
    SST_ELI_DOCUMENT_PARAMS({"rank", "Contribution rank, zero or one", "0"})
    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS({"virtNic", "Host interface to the physical NIC", "SST::Firefly::VirtNic"})

    CollectiveHostTimingTest(ComponentId_t id, Params& params) : Component(id), rank_(params.find<int>("rank", 0))
    {
        nic_ = loadUserSubComponent<VirtNic>("virtNic", ComponentInfo::SHARE_NONE);
        if ( nic_ == nullptr || rank_ < 0 || rank_ > 1 ) fail("invalid fixture configuration");
        start_ = configureSelfLink("start", "1ns",
            new Event::Handler<CollectiveHostTimingTest, &CollectiveHostTimingTest::submit>(this));
        registerAsPrimaryComponent();
        primaryComponentDoNotEndSim();
    }

    void init(unsigned int phase) override { nic_->init(phase); }

    void setup() override
    {
        auto* endpoint = nic_->collectiveEndpoint();
        if ( endpoint == nullptr || !endpoint->bind(*this, *this) ) fail("missing collective endpoint");
        start_->send(1, nullptr);
    }

    void complete(uint64_t invocation, SST::Collective::CollectiveCompletionStatus status) override
    {
        if ( invocation != invocation_ || status != SST::Collective::CollectiveCompletionStatus::Success ||
             value_ != 3.0 ) {
            fail("completion lost the accepted source snapshot or in-place result");
        }
        getSimulationOutput().output(
            "Firefly collective host timing rank=%d invocation=%" PRIu64 " elapsed_ns=%" PRIu64 " in_place=PASS\n",
            rank_, invocation, getCurrentSimTimeNano() - started_at_);
        if ( invocation_ == 2 ) primaryComponentOKToEndSim();
        else start_->send(1, nullptr);
    }

    void ready() override { fail("unexpected unarmed ready notification"); }

private:
    void submit(Event* event)
    {
        delete event;
        ++invocation_;
        value_ = rank_ + 1.0;
        SST::Collective::CollectiveSubmission submission;
        submission.invocation_id = invocation_;
        submission.signature = SST::Collective::STATIC_COLLECTIVE_SIGNATURE_V1;
        submission.source = {reinterpret_cast<const uint8_t*>(&value_), sizeof(value_)};
        submission.result = {reinterpret_cast<uint8_t*>(&value_), sizeof(value_)};
        started_at_ = getCurrentSimTimeNano();
        if ( nic_->submitCollective(submission, 0x1000, 0x1000) !=
             SST::Collective::CollectiveSubmitResult::Accepted ) {
            fail("valid invocation was not accepted");
        }
        // Accepted promises that the source was copied even though DMA is
        // still pending. The caller keeps the shared source/result storage alive.
        value_ = -99.0;
        ++submission.invocation_id;
        if ( nic_->submitCollective(submission, 0x2000, 0x2000) !=
             SST::Collective::CollectiveSubmitResult::Retry || value_ != -99.0 ) {
            fail("an active DMA invocation was not kept exclusive");
        }
    }

    [[noreturn]] void fail(const char* reason) const
    {
        getSimulationOutput().fatal(CALL_INFO, 1, "Firefly collective host timing: %s\n", reason);
    }

    VirtNic* nic_ = nullptr;
    Link* start_ = nullptr;
    int rank_ = 0;
    uint64_t invocation_ = 0;
    uint64_t started_at_ = 0;
    double value_ = 0;
};

} // namespace SST::Firefly
