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
#include <sst/elements/carcosa/Components/HaliEvent.h>
#include <sst/elements/carcosa/Components/InterceptionAgentAPI.h>
#include <sst/elements/carcosa/Components/PipelineStateRegistry.h>
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
        {"num_action_tokens","Hard cap on decode tokens per pipeline cycle.", "1"},
        {"decode_exit_prob","Per-LM_HEAD Bernoulli probability of terminating the decode loop early (EOS-like). 0.0 disables. Range [0.0, 1.0].", "0.0"},
        {"rng_seed",        "Seed for the decode early-exit RNG. Only consumed when decode_exit_prob > 0.", "12345"},
        {"baseline_ps",     "Comma-separated baseline kernel durations in picoseconds per kernel id (from Phase 1 CSV delta_ps column), 18 values.", ""},
        {"baseline_cycles", "[deprecated] Previous name for baseline_ps. Still accepted for one release.", ""},
        {"scale_factor",    "[legacy] Single dimension scale: delay *= scale_factor^complexityOrder(kernel). Used only when all of scale_seq/dim/vocab are 1.0 and scale_factor != 1.0.", "1.0"},
        {"scale_seq",       "Sequence-length scale: target_seq / baseline_seq. Applied to kernels whose cost grows with sequence length. For runtime-sequence kernels (KV_CACHE_ATTN, PREFILL_CAUSAL_ATTN) it is composed with currentSeqLen/baseline_seq_len.", "1.0"},
        {"scale_dim",       "Embedding/projection dim scale: target_dim / baseline_dim.",  "1.0"},
        {"scale_vocab",     "Vocabulary scale for LM_HEAD/DETOK_DEQUANT: target_vocab / baseline_vocab.", "1.0"},
        {"baseline_seq_len","Reference sequence length the baseline_ps were calibrated at; denominator for runtime-sequence kernels. Defaults to initial_seq_len if unset.", "0"},
        {"state_key",       "Optional. PipelineStateRegistry<PipelineStateBase> key this agent publishes into so PortModuleStateGate can read currentKernel/pipelineCycle/regions[]. Empty disables publishing.", ""},
        {"region_size",     "Size in bytes of the published MMIO control region (regions[0]).", "4096"},
        {"verbose",         "Enable verbose output.",                        "false"}
    )

    VLACpuDelayAgent(ComponentId_t id, Params& params);
    VLACpuDelayAgent() : InterceptionAgentAPI() {}
    ~VLACpuDelayAgent();

    bool handleInterceptedEvent(SST::MemHierarchy::MemEvent* ev, SST::Link* highlink) override;
    void handleRingEvent(HaliEvent* ev);
    void agentSetup() override;
    void setRingLink(SST::Link* leftLink) override;
    void setInterceptBase(uint64_t base) override;
    void setHighlink(SST::Link* highlink) override;

    /** Sentinel written into PipelineStateBase::currentKernel between
     *  kernel dispatches. Distinct from VLA's IDLE FSM state (== 0). */
    static constexpr int KERNEL_IDLE = -1;

private:
    void handleDelayComplete(SST::Event* ev);
    void checkBothDone();
    void advanceFSM();
    void sendCommandResponse(SST::MemHierarchy::MemEvent* request, int value);
    void sendWriteAck(SST::MemHierarchy::MemEvent* ev);
    void dispatchToGpu();
    uint64_t computeScaledDelay(int kernelId);
    void publishKernel(int kernel);
    void publishMmioRegion();

    SST::Output* out_;
    SST::Link* highlink_ = nullptr;
    SST::Link* leftHaliLink_ = nullptr;
    SST::Link* selfLink_ = nullptr;
    uint64_t controlAddrBase_ = 0;

    VlaFsm fsm_;

    int nextCommand_ = INT_MIN;
    bool partnerDone_ = false;
    bool localDone_ = false;
    SST::MemHierarchy::MemEvent* pendingCommandRead_ = nullptr;
    bool verbose_ = false;

    uint64_t baselinePs_[NUM_STATES] = {};
    double   scaleFactor_   = 1.0;      // legacy single-factor scale
    double   scaleSeq_      = 1.0;
    double   scaleDim_      = 1.0;
    double   scaleVocab_    = 1.0;
    int      baselineSeqLen_ = 0;       // 0 = use fsm_ initial_seq_len
    bool     legacyScaling_ = false;

    bool delayPending_ = false;

    std::string stateKey_;
    uint64_t    regionSize_ = 4096;

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
