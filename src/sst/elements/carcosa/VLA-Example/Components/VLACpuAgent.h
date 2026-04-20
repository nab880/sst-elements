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

#ifndef CARCOSA_VLACPUAGENT_H
#define CARCOSA_VLACPUAGENT_H

#include <sst/core/link.h>
#include <sst/core/output.h>
#include <sst/core/rng/marsaglia.h>
#include <sst/elements/carcosa/Components/InterceptionAgentAPI.h>
#include <sst/elements/carcosa/VLA-Example/Components/VLAAgent.h>
#include <sst/elements/memHierarchy/memEvent.h>
#include <climits>
#include <cstdint>
#include <string>
#include <vector>

namespace SST {
namespace Carcosa {

/** CPU FSM master: ring to GPU (cmd, seqlen, exit) / from GPU (done). MMIO +0 cmd, +4 status, +8 seqlen, +10 role=0. */
class VLACpuAgent : public InterceptionAgentAPI
{
public:
    SST_ELI_REGISTER_SUBCOMPONENT(
        VLACpuAgent,
        "Carcosa",
        "VLACpuAgent",
        SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "CPU-side VLA FSM master agent; coordinates with VLAGpuAgent via Hali ring",
        SST::Carcosa::InterceptionAgentAPI
    )

    SST_ELI_DOCUMENT_PARAMS(
        {"num_vit_layers",  "Number of ViT layers (L).",                     "24"},
        {"num_llm_layers",  "Number of LLM layers (L_LLM).",                 "32"},
        {"max_cycles",      "Pipeline cycles before exit. 0 = forever.",     "1"},
        {"initial_seq_len", "Sequence length after prefill.",                 "228"},
        {"max_seq_len",     "KV-cache capacity in the RISC-V binary (must match MAX_SEQ_LEN in vla_shared.h). Fatal if initial_seq_len + (num_action_tokens - 1) > max_seq_len.", "64"},
        {"num_action_tokens","Decode tokens generated per pipeline cycle.",   "1"},
        {"verbose",         "Enable verbose output.",                        "false"}
    )

    VLACpuAgent(ComponentId_t id, Params& params);
    VLACpuAgent() : InterceptionAgentAPI() {}
    ~VLACpuAgent();

    bool handleInterceptedEvent(SST::MemHierarchy::MemEvent* ev, SST::Link* highlink) override;
    void handleRingEvent(HaliEvent* ev) override;
    void agentSetup() override;
    void setRingLink(SST::Link* leftLink) override;
    void setInterceptBase(uint64_t base) override;
    void setHighlink(SST::Link* highlink) override;

private:
    void checkBothDone();
    void advanceFSM();
    void sendCommandResponse(SST::MemHierarchy::MemEvent* request, int value);
    void sendWriteAck(SST::MemHierarchy::MemEvent* ev);
    void dispatchToGpu();

    SST::Output* out_;
    SST::Link* highlink_ = nullptr;
    SST::Link* leftHaliLink_ = nullptr;
    uint64_t controlAddrBase_ = 0;

    VLAState currentState_ = IDLE;
    int vitLayer_ = 0;
    int prefillLayer_ = 0;
    int decodeLayer_ = 0;
    int currentSeqLen_ = 0;
    int actionTokenCount_ = 0;
    int numViTLayers_ = 24;
    int numLLMLayers_ = 32;
    int maxCycles_ = 1;
    int initialSeqLen_ = 228;
    int maxSeqLen_ = 64;
    int numActionTokens_ = 1;
    int pipelineCycles_ = 0;
    bool exitAfterThisRead_ = false;
    SST::RNG::MarsagliaRNG* rng_ = nullptr;

    int nextCommand_ = INT_MIN;
    bool partnerDone_ = false;
    bool localDone_ = false;
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

#endif /* CARCOSA_VLACPUAGENT_H */
