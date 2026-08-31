// Copyright 2013-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2013-2026, NTESS
// All rights reserved.
//
// Portions are copyright of other developers:
// See the file CONTRIBUTORS.TXT in the top level directory
// of the distribution for more information.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#ifndef COMPONENTS_FIREFLY_FUNCSM_ALLREDUCE_H
#define COMPONENTS_FIREFLY_FUNCSM_ALLREDUCE_H

#include "funcSM/collectiveTree.h"
#include "sst/elements/merlin/services/collective/collectiveEndpoint.h"

#include <cstdint>
#include <optional>

namespace SST {
namespace Firefly {

class AllreduceFuncSM : public CollectiveTreeFuncSM
{
  public:
    SST_ELI_REGISTER_MODULE(
        AllreduceFuncSM,
        "firefly",
        "Allreduce",
        SST_ELI_ELEMENT_VERSION(1,0,0),
        "Software tree Allreduce",
        SST::Firefly::CollectiveTreeFuncSM
    )

    explicit AllreduceFuncSM(SST::Params& params) : CollectiveTreeFuncSM(params) {}

    void handleStartEvent(SST::Event* event, Retval& retval) override
    {
        CollectiveTreeFuncSM::handleStartEvent(event, retval);
    }
    void handleEnterEvent(Retval& retval) override
    {
        CollectiveTreeFuncSM::handleEnterEvent(retval);
    }
    std::string protocolName() override { return "CtrlMsgProtocol"; }
};

static_assert(sizeof(AllreduceFuncSM) == sizeof(CollectiveTreeFuncSM),
    "Disabled Allreduce must not add per-rank state");

/**
 * Opt-in selector around the existing software tree. The initial offload
 * profile is intentionally narrow: one backed F64 SUM element on GroupWorld
 * and one published participant per physical NIC.
 */
class AllreduceOffloadFuncSM : public CollectiveTreeFuncSM,
                               public SST::Collective::CollectiveCompletionSink,
                               public SST::Collective::CollectiveReadySink
{
  public:
    SST_ELI_REGISTER_MODULE(
        AllreduceOffloadFuncSM,
        "firefly",
        "AllreduceOffload",
        SST_ELI_ELEMENT_VERSION(1,0,0),
        "Allreduce with an optional static in-network SUM/F64 path",
        SST::Firefly::CollectiveTreeFuncSM
    )

    SST_ELI_DOCUMENT_PARAMS(
        {"enableOffload", "Enable the static collective endpoint", "false"},
        {"forceSoftware", "Use the existing software tree before service injection", "false"},
        {"reportOffload", "Report the selected path once per call", "false"},
    )

    explicit AllreduceOffloadFuncSM(SST::Params& params);
    ~AllreduceOffloadFuncSM() override;

    void handleStartEvent(SST::Event* event, Retval& retval) override;
    void handleEnterEvent(Retval& retval) override;
    std::string protocolName() override { return "CtrlMsgProtocol"; }

    void complete(SST::Collective::CollectiveCompletionToken&& token,
        SST::Collective::CollectiveCompletionStatus status) override;
    void ready(const SST::Collective::AcceptedParticipantHandle& participant) override;

  private:
    friend class AllreduceErrorRegressionTest;

    enum class Mode : uint8_t { Idle, Software, WaitingReady, WaitingCompletion };

    static std::optional<SST::Collective::CollectiveSignatureV1> translateSignature(
        const CollectiveStartEvent& event);
    void startSoftware(CollectiveStartEvent* event, Retval& retval);
    bool bindOffload();
    void startOffload(CollectiveStartEvent* event,
        const SST::Collective::CollectiveSignatureV1& signature,
        uint64_t invocation_id, Retval& retval);
    void submitOffload(Retval& retval);
    void scheduleResume();
    CollectiveStartEvent* finishOffload();
    void reportPath(const char* path) const;
    void fail(const char* reason);

    CtrlMsg::API* collectiveProtocol() const;

    bool enable_offload_ = false;
    bool force_software_ = false;
    bool report_offload_ = false;
    bool bound_ = false;
    bool ready_received_ = false;
    bool completion_received_ = false;
    bool wake_scheduled_ = false;

    Mode mode_ = Mode::Idle;
    uint64_t next_invocation_id_ = 1;
    uint64_t active_invocation_id_ = 0;
    SST::Collective::CollectiveCompletionStatus completion_status_ =
        SST::Collective::CollectiveCompletionStatus::RecoverableError;

    CollectiveStartEvent* offload_event_ = nullptr;
    SST::Collective::CollectiveEndpoint* endpoint_ = nullptr;
    const SST::Collective::AcceptedParticipantHandle* participant_ = nullptr;
    SST::Collective::CollectivePending pending_;
};

}
}

#endif
