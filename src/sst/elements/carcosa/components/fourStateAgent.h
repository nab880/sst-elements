// Copyright 2009-2025 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2025, NTESS
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#ifndef CARCOSA_FOURSTATEAGENT_H
#define CARCOSA_FOURSTATEAGENT_H

#include <sst/core/link.h>
#include <sst/core/output.h>
#include <sst/elements/carcosa/components/interceptionAgentAPI.h>
#include <sst/elements/carcosa/components/pipelineStateRegistry.h>
#include <sst/elements/memHierarchy/memEvent.h>
#include <climits>
#include <cstdint>
#include <string>

namespace SST {
namespace Carcosa {

/**
 * InterceptionAgent that speaks the same MMIO ping/pong protocol as
 * PingPongAgent (so the existing hyades.h ABI works unchanged) but
 * generalizes the command sequence from 2 handlers (ping/pong) to
 * `num_commands` handlers, and publishes a live snapshot into
 * PipelineStateRegistry<PipelineStateBase> under a configurable `state_key`.
 *
 * Published snapshot semantics:
 *   currentKernel : the command index currently executing on the CPU.
 *                   Set to the command value at the instant the response
 *                   is sent back to the core, and reset to -1 when the
 *                   core's status-write arrives (meaning the CPU is idle
 *                   between kernels). A PortModule with kernels="2" will
 *                   therefore match exactly the window during which the
 *                   CPU is running handler index 2.
 *   pipelineCycle : number of completed full passes through all
 *                   `num_commands` kernels (i.e. floor(currentIteration /
 *                   num_commands)).
 *   regions[0]    : the MMIO control region, name="mmio_control",
 *                   base=<Hali intercept base>, size=region_size.
 *
 * Test purpose: exercise PipelineStateRegistry against the full Vanadis +
 * memHierarchy + Hali stack (see tests/testfourStateRegistry.py) and
 * demonstrate a PortModuleStateGate gating cache traffic on the published
 * currentKernel (see tests/testfourStateRegistryGated.py).
 */
class FourStateAgent : public InterceptionAgentAPI
{
public:
    SST_ELI_REGISTER_SUBCOMPONENT(
        FourStateAgent,
        "Carcosa",
        "FourStateAgent",
        SST_ELI_ELEMENT_VERSION(0, 1, 0),
        "N-kernel MMIO agent that publishes PipelineStateBase (currentKernel = CPU command index in flight).",
        SST::Carcosa::InterceptionAgentAPI
    )

    SST_ELI_DOCUMENT_PARAMS(
        {"state_key",       "PipelineStateRegistry key this agent publishes into. Required.", ""},
        {"region_size",     "Size in bytes of the published MMIO control region (regions[0]).", "4096"},
        {"initial_command", "First command index returned to the CPU.", "0"},
        {"num_commands",    "Number of command indices to cycle through (must match the jump_table length in the Vanadis binary; 2 for pingpong, 4 for fourstate).", "4"},
        {"max_iterations",  "Max iterations before sending exit (-1).",  "12"},
        {"verbose",         "Enable verbose per-event output.", "false"}
    )

    FourStateAgent(ComponentId_t id, Params& params);
    FourStateAgent() : InterceptionAgentAPI() {}
    ~FourStateAgent() override;

    bool handleInterceptedEvent(SST::MemHierarchy::MemEvent* ev, SST::Link* highlink) override;
    void notifyPartnerDone(unsigned iteration) override;
    void agentSetup() override;
    void setRingLink(SST::Link* leftLink) override;
    void setInterceptBase(uint64_t base) override;
    void setHighlink(SST::Link* highlink) override;

    /** Sentinel value written into PipelineStateBase::currentKernel when
     *  the CPU is idle (i.e. between the status-write of one kernel and the
     *  command-read that dispatches the next). Any predicate with a
     *  positive kernel set will correctly not match this value. */
    static constexpr int IDLE = -1;

private:
    void checkBothDone();
    void sendCommandResponse(SST::MemHierarchy::MemEvent* request, int value);
    void sendWriteAck(SST::MemHierarchy::MemEvent* ev);

    /** Write the current snapshot (currentKernel, pipelineCycle) into the
     *  registry entry for stateKey_. Creates the entry if it does not yet
     *  exist (defensive; agentSetup() normally creates it first). */
    void publishState(int kernel);

    SST::Output* out_        = nullptr;
    SST::Link*   leftHaliLink_ = nullptr;
    SST::Link*   highlink_     = nullptr;

    // Ping/pong-compatible protocol state (kept identical so hyades.h works)
    uint64_t controlAddrBase_ = 0;
    uint64_t regionSize_      = 4096;
    int      initialCommand_  = 0;
    int      numCommands_     = 4;
    int      maxIterations_   = 12;
    int      currentIteration_ = 0;
    int      nextCommand_      = INT_MIN;
    bool     partnerDone_      = false;
    bool     localDone_        = false;
    bool     verbose_          = false;
    SST::MemHierarchy::MemEvent* pendingCommandRead_ = nullptr;

    // Registry publishing state
    std::string stateKey_;
    int         publishedKernel_ = IDLE;
};

} // namespace Carcosa
} // namespace SST

#endif /* CARCOSA_FOURSTATEAGENT_H */
