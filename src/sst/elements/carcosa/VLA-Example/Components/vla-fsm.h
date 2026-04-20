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

/**
 * Shared VLA pipeline FSM used by VLAAgent, VLACpuAgent, and VLACpuDelayAgent.
 *
 * Owns the state + layer/token/seqlen counters and the IDLE->ACTUATE->IDLE
 * transition table. Agents embed a VlaFsm and delegate advance()/reset()
 * to it so there is a single place to fix/unit-test the state machine.
 *
 * Logging is the caller's responsibility: read state() before and after
 * advance() if you want the old "prev -> next" trace with your agent's
 * name prefix.
 *
 * Fatal errors (peak-seqlen / KV-cache overflow) are reported through the
 * caller-supplied SST::Output* so the agent-specific prefix is preserved.
 */
class VlaFsm {
public:
    struct Config {
        int numViTLayers    = 24;
        int numLLMLayers    = 32;
        int maxCycles       = 1;
        int initialSeqLen   = 228;
        int maxSeqLen       = 64;
        int numActionTokens = 1;
    };

    VlaFsm() = default;
    explicit VlaFsm(const Config& cfg) : cfg_(cfg) {}

    void setConfig(const Config& cfg) { cfg_ = cfg; }
    const Config& config() const { return cfg_; }

    /** Reset counters and put the FSM in IDLE. Does not touch Config or exit flag state
     *  beyond clearing it; intended to be called from agentSetup(). */
    void reset();

    /** Fatal (via `out`) if initial_seq_len + (num_action_tokens - 1) > max_seq_len. */
    void validatePeakSeqLen(SST::Output* out, const char* agentName) const;

    /** Advance one FSM step. Returns the previous state so callers can log
     *  "prev -> next" with their own prefix. Fatal (via `out`) on KV-cache overflow
     *  in LM_HEAD.  */
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
    Config   cfg_{};
    VLAState currentState_      = IDLE;
    int      vitLayer_          = 0;
    int      prefillLayer_      = 0;
    int      decodeLayer_       = 0;
    int      currentSeqLen_     = 0;
    int      actionTokenCount_  = 0;
    int      pipelineCycles_    = 0;
    bool     exitAfterThisRead_ = false;
};

} // namespace Carcosa
} // namespace SST

#endif /* CARCOSA_VLA_FSM_H */
