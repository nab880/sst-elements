// Copyright 2009-2024 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2024, NTESS
// All rights reserved.
//
// Portions are copyright of other developers:
// See the file CONTRIBUTORS.TXT in the top level directory of the distribution.
//
// This file is part of the SST software package. For license information,
// see the LICENSE file in the top level directory of the distribution.

#ifndef CARCOSA_VLAGPUAGENT_H
#define CARCOSA_VLAGPUAGENT_H

#include <sst/core/link.h>
#include <sst/core/output.h>
#include <sst/elements/carcosa/Components/InterceptionAgentAPI.h>
#include <sst/elements/memHierarchy/memEvent.h>
#include <climits>
#include <cstdint>
#include <string>
#include <vector>

namespace SST {
namespace Carcosa {

/** GPU follower: ring in (seqlen, cmd, exit), out (done). MMIO +0 cmd, +4 status, +8 seqlen, +10 role=1. */
class VLAGpuAgent : public InterceptionAgentAPI
{
public:
    SST_ELI_REGISTER_SUBCOMPONENT(
        VLAGpuAgent,
        "Carcosa",
        "VLAGpuAgent",
        SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "VLA GPU follower (ring from CPU)",
        SST::Carcosa::InterceptionAgentAPI
    )

    SST_ELI_DOCUMENT_PARAMS(
        {"max_seq_len", "Binary KV cap; match MAX_SEQ_LEN. Fatal if ring seqlen exceeds.", "64"},
        {"verbose",     "Verbose logging.", "false"}
    )

    VLAGpuAgent(ComponentId_t id, Params& params);
    VLAGpuAgent() : InterceptionAgentAPI() {}
    ~VLAGpuAgent();

    bool handleInterceptedEvent(SST::MemHierarchy::MemEvent* ev, SST::Link* highlink) override;
    void handleRingEvent(HaliEvent* ev) override;
    void agentSetup() override;
    void setRingLink(SST::Link* leftLink) override;
    void setInterceptBase(uint64_t base) override;
    void setHighlink(SST::Link* highlink) override;

private:
    void sendCommandResponse(SST::MemHierarchy::MemEvent* request, int value);
    void sendWriteAck(SST::MemHierarchy::MemEvent* ev);

    SST::Output* out_;
    SST::Link* highlink_ = nullptr;
    SST::Link* leftHaliLink_ = nullptr;
    uint64_t controlAddrBase_ = 0;

    int nextCommand_ = INT_MIN;
    int seqLen_ = 0;
    int maxSeqLen_ = 64;
    SST::MemHierarchy::MemEvent* pendingCommandRead_ = nullptr;
    bool verbose_ = false;

    struct KernelRecord {
        std::string core;
        int kernelId;
        std::string kernelName;
        uint64_t startCycle;
        uint64_t endCycle;
    };
    std::vector<KernelRecord> profile_;
    uint64_t kernelStartCycle_ = 0;
    int activeKernelId_ = -1;

    void recordKernelEnd();
    void printProfile();
};

} // namespace Carcosa
} // namespace SST

#endif /* CARCOSA_VLAGPUAGENT_H */
