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

#ifndef CARCOSA_VLACPUDELAYAGENT_H
#define CARCOSA_VLACPUDELAYAGENT_H

#include <sst/core/event.h>
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

/** Phase 2 CPU: same FSM/ring as VLACpuAgent; kernel time from Phase1 baseline_ps (ps) on 1ps self-link; advance when local delay and partner done. */
class VLACpuDelayAgent : public InterceptionAgentAPI
{
public:
    SST_ELI_REGISTER_SUBCOMPONENT(
        VLACpuDelayAgent,
        "Carcosa",
        "VLACpuDelayAgent",
        SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "Phase 2 CPU delay agent; injects analytical kernel delays",
        SST::Carcosa::InterceptionAgentAPI
    )

    SST_ELI_DOCUMENT_PARAMS(
        {"num_vit_layers",  "Number of ViT layers.",                         "24"},
        {"num_llm_layers",  "Number of LLM layers.",                         "32"},
        {"max_cycles",      "Pipeline cycles before exit. 0 = forever.",     "1"},
        {"initial_seq_len", "Sequence length after prefill.",                 "228"},
        {"max_seq_len",     "KV-cache capacity in the stub/real binary (MAX_SEQ_LEN). Fatal if initial_seq_len + (num_action_tokens - 1) > max_seq_len.", "64"},
        {"num_action_tokens","Decode tokens generated per pipeline cycle.",   "1"},
        {"baseline_ps",     "Comma-separated baseline kernel durations in picoseconds per kernel id (from Phase 1 CSV delta_ps column), 18 values.", ""},
        {"baseline_cycles", "[deprecated] Previous name for baseline_ps. Still accepted for one release.", ""},
        {"scale_factor",    "Dimension scale factor: target_dim / baseline_dim.", "1.0"},
        {"verbose",         "Enable verbose output.",                        "false"}
    )

    VLACpuDelayAgent(ComponentId_t id, Params& params);
    VLACpuDelayAgent() : InterceptionAgentAPI() {}
    ~VLACpuDelayAgent();

    bool handleInterceptedEvent(SST::MemHierarchy::MemEvent* ev, SST::Link* highlink) override;
    void handleRingEvent(HaliEvent* ev) override;
    void agentSetup() override;
    void setRingLink(SST::Link* leftLink) override;
    void setInterceptBase(uint64_t base) override;
    void setHighlink(SST::Link* highlink) override;

private:
    void handleDelayComplete(SST::Event* ev);
    void checkBothDone();
    void advanceFSM();
    void sendCommandResponse(SST::MemHierarchy::MemEvent* request, int value);
    void sendWriteAck(SST::MemHierarchy::MemEvent* ev);
    void dispatchToGpu();
    uint64_t computeScaledDelay(int kernelId);

    SST::Output* out_;
    SST::Link* highlink_ = nullptr;
    SST::Link* leftHaliLink_ = nullptr;
    SST::Link* selfLink_ = nullptr;
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

    uint64_t baselinePs_[NUM_STATES] = {};
    double scaleFactor_ = 1.0;

    bool delayPending_ = false;

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

#endif /* CARCOSA_VLACPUDELAYAGENT_H */
