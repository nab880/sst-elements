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

#ifndef CARCOSA_VLAGPUDELAYAGENT_H
#define CARCOSA_VLAGPUDELAYAGENT_H

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

/** Phase 2 GPU: ring like VLAGpuAgent; baseline_ps on 1ps self-link before sending done. */
class VLAGpuDelayAgent : public InterceptionAgentAPI
{
public:
    SST_ELI_REGISTER_SUBCOMPONENT(
        VLAGpuDelayAgent,
        "Carcosa",
        "VLAGpuDelayAgent",
        SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "Phase 2 GPU delay agent; injects analytical kernel delays",
        SST::Carcosa::InterceptionAgentAPI
    )

    SST_ELI_DOCUMENT_PARAMS(
        {"baseline_ps",     "Comma-separated baseline kernel durations in picoseconds per kernel id (from Phase 1 CSV delta_ps column), 18 values.", ""},
        {"baseline_cycles", "[deprecated] Previous name for baseline_ps. Still accepted for one release.", ""},
        {"scale_factor",    "[legacy] Single dimension scale: delay *= scale_factor^complexityOrder(kernel). Used only when all of scale_seq/dim/vocab are 1.0 and scale_factor != 1.0.", "1.0"},
        {"scale_seq",       "Sequence-length scale. For runtime-sequence kernels (KV_CACHE_ATTN, PREFILL_CAUSAL_ATTN) composed with ring-delivered currentSeqLen/baseline_seq_len.", "1.0"},
        {"scale_dim",       "Embedding/projection dim scale: target_dim / baseline_dim.", "1.0"},
        {"scale_vocab",     "Vocabulary scale for LM_HEAD/DETOK_DEQUANT.",                "1.0"},
        {"baseline_seq_len","Reference sequence length the baseline_ps were calibrated at; denominator for runtime-sequence kernels.", "228"},
        {"max_seq_len",     "KV-cache capacity in the stub/real binary (MAX_SEQ_LEN). Fatal if the CPU delay agent ever announces a seqlen > max_seq_len on the ring.", "64"},
        {"state_key",       "Optional. PipelineStateRegistry<PipelineStateBase> key this agent publishes into so PortModuleStateGate can read currentKernel/pipelineCycle/regions[]. Empty disables publishing.", ""},
        {"region_size",     "Size in bytes of the published MMIO control region (regions[0]).", "4096"},
        {"regions",         "Optional CSV of workload-labeled DRAM regions for region-aware EccGuard policies. 'name:base:size' triples; slot 0 reserved for mmio_control.", ""},
        {"verbose",         "Enable verbose output.",                             "false"}
    )

    VLAGpuDelayAgent(ComponentId_t id, Params& params);
    VLAGpuDelayAgent() : InterceptionAgentAPI() {}
    ~VLAGpuDelayAgent();

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
    void sendCommandResponse(SST::MemHierarchy::MemEvent* request, int value);
    void sendWriteAck(SST::MemHierarchy::MemEvent* ev);
    uint64_t computeScaledDelay(int kernelId);
    void publishKernel(int kernel);
    void publishMmioRegion();
    void applyRegionPublish(uint64_t offset, uint32_t value);

    SST::Output* out_;
    SST::Link* highlink_ = nullptr;
    SST::Link* leftHaliLink_ = nullptr;
    SST::Link* selfLink_ = nullptr;
    uint64_t controlAddrBase_ = 0;

    int nextCommand_ = INT_MIN;
    int seqLen_ = 0;
    int maxSeqLen_ = 64;
    SST::MemHierarchy::MemEvent* pendingCommandRead_ = nullptr;
    bool verbose_ = false;

    uint64_t baselinePs_[NUM_STATES] = {};
    double   scaleFactor_   = 1.0;      // legacy
    double   scaleSeq_      = 1.0;
    double   scaleDim_      = 1.0;
    double   scaleVocab_    = 1.0;
    int      baselineSeqLen_ = 228;
    bool     legacyScaling_ = false;
    bool delayPending_ = false;

    std::string stateKey_;
    uint64_t    regionSize_ = 4096;
    std::string regionsCsv_;
    // GPU has no FSM; advance pipelineCycle each completed ACTUATE.
    int gpuPipelineCycle_ = 0;

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

#endif /* CARCOSA_VLAGPUDELAYAGENT_H */
