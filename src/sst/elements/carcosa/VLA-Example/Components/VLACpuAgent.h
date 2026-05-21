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
        {"num_action_tokens","Hard cap on decode tokens per pipeline cycle.", "1"},
        {"decode_exit_prob","Per-LM_HEAD Bernoulli probability of terminating the decode loop early (EOS-like). 0.0 disables. Range [0.0, 1.0].", "0.0"},
        {"rng_seed",        "Seed for the decode early-exit RNG. Only consumed when decode_exit_prob > 0.", "12345"},
        {"state_key",       "Optional. PipelineStateRegistry<PipelineStateBase> key this agent publishes into so PortModuleStateGate (or any consumer) can read currentKernel/pipelineCycle/regions[]. Empty disables publishing.", ""},
        {"region_size",     "Size in bytes of the published MMIO control region (regions[0]).", "4096"},
        {"regions",         "Optional CSV of workload-labeled DRAM regions for region-aware EccGuard policies. 'name:base:size' triples; slot 0 reserved for mmio_control.", ""},
        {"verbose",         "Enable verbose output.",                        "false"}
    )

    VLACpuAgent(ComponentId_t id, Params& params);
    VLACpuAgent() : InterceptionAgentAPI() {}
    ~VLACpuAgent();

    bool handleInterceptedEvent(SST::MemHierarchy::MemEvent* ev, SST::Link* highlink) override;
    void handleRingEvent(HaliEvent* ev);
    void agentSetup() override;
    void setRingLink(SST::Link* leftLink) override;
    void setInterceptBase(uint64_t base) override;
    void setHighlink(SST::Link* highlink) override;

    /** Sentinel written into PipelineStateBase::currentKernel when the
     *  CPU is between kernels (status-write received, no kernel dispatched
     *  yet). Distinct from VLA's IDLE FSM state (== 0). */
    static constexpr int KERNEL_IDLE = -1;

private:
    void checkBothDone();
    void advanceFSM();
    void sendCommandResponse(SST::MemHierarchy::MemEvent* request, int value);
    void sendWriteAck(SST::MemHierarchy::MemEvent* ev);
    void dispatchToGpu();
    void publishKernel(int kernel);
    void publishMmioRegion();
    // Region-publish ABI handler (HYADES_REGION_* offsets 0x20/0x24/0x28/0x2C).
    // The workload binary writes base_lo/base_hi/size/commit-slot at these
    // offsets so EccGuard's region-aware policy sees the same virtual
    // addresses Vanadis touches. Mirrors VLACpuDelayAgent::applyRegionPublish.
    void applyRegionPublish(uint64_t offset, uint32_t value);

    SST::Output* out_;
    SST::Link* highlink_ = nullptr;
    SST::Link* leftHaliLink_ = nullptr;
    uint64_t controlAddrBase_ = 0;

    VlaFsm fsm_;

    int nextCommand_ = INT_MIN;
    bool partnerDone_ = false;
    bool localDone_ = false;
    SST::MemHierarchy::MemEvent* pendingCommandRead_ = nullptr;
    bool verbose_ = false;

    std::string stateKey_;
    uint64_t    regionSize_ = 4096;
    std::string regionsCsv_;

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

    // Edge-trigger cache for per-frame `dropped`; see VLACpuDelayAgent.h.
    int lastFramesDroppedSeen_ = 0;

    // Workload-published action checksum (HYADES_ACTION_CHECKSUM_OFFSET 0x0030).
    uint32_t latestActionChecksum_    = 0;
    bool     latestActionChecksumSet_ = false;

    void recordKernelEnd();
    void printProfile();
};

} // namespace Carcosa
} // namespace SST

#endif /* CARCOSA_VLACPUAGENT_H */
