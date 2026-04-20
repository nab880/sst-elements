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

#ifndef CARCOSA_VLA_FSM_H
#define CARCOSA_VLA_FSM_H

#include <sst/core/output.h>
#include <sst/core/rng/marsaglia.h>
#include <cstdint>

namespace SST {
namespace Carcosa {

enum VLAState {
    IDLE,
    VISION_INGESTION,
    PATCHIFICATION_EMBED,
    VIS_ATTN_PROJ,
    GLOBAL_SPATIAL_ATTN,
    VIS_FFN,
    MLP_PROJECTOR,
    SEQ_CONCAT,
    PREFILL_ATTN_PROJ,
    PREFILL_CAUSAL_ATTN,
    PREFILL_FFN,
    GEMV_PROJECT,
    KV_CACHE_ATTN,
    DECODE_FFN,
    LM_HEAD,
    DETOK_DEQUANT,
    FAST_IDCT,
    ACTUATE,
    NUM_STATES
};

inline const char* vlaStateName(int id) {
    static const char* names[NUM_STATES] = {
        "IDLE", "VISION_INGESTION", "PATCHIFICATION_EMBED",
        "VIS_ATTN_PROJ", "GLOBAL_SPATIAL_ATTN", "VIS_FFN",
        "MLP_PROJECTOR", "SEQ_CONCAT", "PREFILL_ATTN_PROJ",
        "PREFILL_CAUSAL_ATTN", "PREFILL_FFN", "GEMV_PROJECT",
        "KV_CACHE_ATTN", "DECODE_FFN", "LM_HEAD",
        "DETOK_DEQUANT", "FAST_IDCT", "ACTUATE"
    };
    return (id >= 0 && id < NUM_STATES) ? names[id] : "UNKNOWN";
}

 // Shared VLA pipeline FSM used by VLAAgent, VLACpuAgent, and VLACpuDelayAgent.
 
class VlaFsm {
public:
    struct Config {
        int      numViTLayers        = 24;
        int      numLLMLayers        = 32;
        int      maxCycles           = 1;
        int      initialSeqLen       = 228;
        int      maxSeqLen           = 64;
        int      numActionTokens     = 1;
        // Per-LM_HEAD Bernoulli early-exit probability for the decode loop.
        double   decodeEarlyExitProb = 0.0;
        uint32_t rngSeed             = 12345u;
    };

    VlaFsm() = default;
    explicit VlaFsm(const Config& cfg) : cfg_(cfg) {}

    void setConfig(const Config& cfg) { cfg_ = cfg; }
    const Config& config() const { return cfg_; }


    void reset();

    void validatePeakSeqLen(SST::Output* out, const char* agentName) const;

    // Advance one FSM step.
    VLAState advance(SST::Output* out, const char* agentName);

    VLAState state()            const { return currentState_; }
    int      currentSeqLen()    const { return currentSeqLen_; }
    int      vitLayer()         const { return vitLayer_; }
    int      prefillLayer()     const { return prefillLayer_; }
    int      decodeLayer()      const { return decodeLayer_; }
    int      actionTokenCount() const { return actionTokenCount_; }
    int      pipelineCycles()   const { return pipelineCycles_; }
    bool     exitAfterThisRead()const { return exitAfterThisRead_; }
    void     clearExitFlag()          { exitAfterThisRead_ = false; }

private:
    Config                cfg_{};
    VLAState              currentState_      = IDLE;
    int                   vitLayer_          = 0;
    int                   prefillLayer_      = 0;
    int                   decodeLayer_       = 0;
    int                   currentSeqLen_     = 0;
    int                   actionTokenCount_  = 0;
    int                   pipelineCycles_    = 0;
    bool                  exitAfterThisRead_ = false;
    SST::RNG::MarsagliaRNG rng_{};
};

} // namespace Carcosa
} // namespace SST

#endif /* CARCOSA_VLA_FSM_H */
