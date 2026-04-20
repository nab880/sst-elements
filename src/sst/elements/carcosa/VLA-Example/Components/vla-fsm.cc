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

#include "sst_config.h"
#include "sst/elements/carcosa/VLA-Example/Components/vla-fsm.h"

namespace SST {
namespace Carcosa {

void VlaFsm::reset()
{
    currentState_      = IDLE;
    vitLayer_          = 0;
    prefillLayer_      = 0;
    decodeLayer_       = 0;
    currentSeqLen_     = 0;
    actionTokenCount_  = 0;
    pipelineCycles_    = 0;
    exitAfterThisRead_ = false;
}

void VlaFsm::validatePeakSeqLen(SST::Output* out, const char* agentName) const
{
    int peak = cfg_.initialSeqLen + (cfg_.numActionTokens - 1);
    if (peak > cfg_.maxSeqLen) {
        out->fatal(CALL_INFO, -1,
            "%s: peak sequence length %d (initial_seq_len=%d + num_action_tokens-1=%d) "
            "exceeds max_seq_len=%d. Binary KV-cache would overflow.\n",
            agentName, peak, cfg_.initialSeqLen, cfg_.numActionTokens - 1, cfg_.maxSeqLen);
    }
}

VLAState VlaFsm::advance(SST::Output* out, const char* agentName)
{
    VLAState prev = currentState_;
    VLAState next = currentState_;

    switch (currentState_) {
    case IDLE:                 next = VISION_INGESTION;     break;
    case VISION_INGESTION:     next = PATCHIFICATION_EMBED; break;
    case PATCHIFICATION_EMBED: next = VIS_ATTN_PROJ;        break;
    case VIS_ATTN_PROJ:        next = GLOBAL_SPATIAL_ATTN;  break;
    case GLOBAL_SPATIAL_ATTN:  next = VIS_FFN;              break;
    case VIS_FFN:
        vitLayer_++;
        next = (vitLayer_ < cfg_.numViTLayers) ? VIS_ATTN_PROJ : MLP_PROJECTOR;
        break;
    case MLP_PROJECTOR:        next = SEQ_CONCAT;           break;
    case SEQ_CONCAT:
        next = PREFILL_ATTN_PROJ;
        currentSeqLen_ = cfg_.initialSeqLen;
        break;
    case PREFILL_ATTN_PROJ:    next = PREFILL_CAUSAL_ATTN;  break;
    case PREFILL_CAUSAL_ATTN:  next = PREFILL_FFN;          break;
    case PREFILL_FFN:
        prefillLayer_++;
        next = (prefillLayer_ < cfg_.numLLMLayers) ? PREFILL_ATTN_PROJ : GEMV_PROJECT;
        break;
    case GEMV_PROJECT:         next = KV_CACHE_ATTN;        break;
    case KV_CACHE_ATTN:        next = DECODE_FFN;           break;
    case DECODE_FFN:
        decodeLayer_++;
        if (decodeLayer_ < cfg_.numLLMLayers) {
            next = GEMV_PROJECT;
        } else {
            next = LM_HEAD;
            decodeLayer_ = 0;
        }
        break;
    case LM_HEAD: {
        actionTokenCount_++;
        if (actionTokenCount_ < cfg_.numActionTokens) {
            if (currentSeqLen_ + 1 > cfg_.maxSeqLen) {
                out->fatal(CALL_INFO, -1,
                    "%s: currentSeqLen_ would become %d and exceed max_seq_len=%d; "
                    "binary KV-cache overflow would occur.\n",
                    agentName, currentSeqLen_ + 1, cfg_.maxSeqLen);
            }
            currentSeqLen_++;
            decodeLayer_ = 0;
            next = GEMV_PROJECT;
        } else {
            actionTokenCount_ = 0;
            next = DETOK_DEQUANT;
        }
        break;
    }
    case DETOK_DEQUANT:        next = FAST_IDCT;            break;
    case FAST_IDCT:            next = ACTUATE;              break;
    case ACTUATE:
        next = IDLE;
        vitLayer_         = 0;
        prefillLayer_     = 0;
        decodeLayer_      = 0;
        actionTokenCount_ = 0;
        pipelineCycles_++;
        if (cfg_.maxCycles > 0 && pipelineCycles_ >= cfg_.maxCycles)
            exitAfterThisRead_ = true;
        break;
    default: break;
    }

    currentState_ = next;
    return prev;
}

} // namespace Carcosa
} // namespace SST
