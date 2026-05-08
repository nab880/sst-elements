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

#ifndef CARCOSA_VLA_KERNEL_COMPLEXITY_H
#define CARCOSA_VLA_KERNEL_COMPLEXITY_H

#include <sst/core/output.h>
#include <sst/elements/carcosa/VLA-Example/Components/VLAAgent.h>
#include <cctype>
#include <cinttypes>
#include <cstdint>
#include <exception>
#include <sstream>
#include <string>

namespace SST {
namespace Carcosa {

/* Per-kernel scale_factor exponent (GEMM-like^3, attn/GEMV^2, linear^1, else^0). Shared header for delay agents. */
inline int complexityOrder(int kernelId)
{
    switch (kernelId) {
    case PATCHIFICATION_EMBED:
    case VIS_ATTN_PROJ:
    case PREFILL_ATTN_PROJ:
    case VIS_FFN:
    case PREFILL_FFN:
    case DECODE_FFN:
    case MLP_PROJECTOR:
        return 3;
    case GLOBAL_SPATIAL_ATTN:
    case PREFILL_CAUSAL_ATTN:
    case KV_CACHE_ATTN:
    case GEMV_PROJECT:
    case LM_HEAD:
        return 2;
    case VISION_INGESTION:
    case SEQ_CONCAT:
    case DETOK_DEQUANT:
    case FAST_IDCT:
        return 1;
    default:
        return 0;
    }
}

/* Per-kernel cost-driver exponents: delay = base * effSeq^seq * scaleDim^dim * scaleVocab^vocab,
 * where effSeq = scaleSeq * (runtimeSeq ? currentSeqLen/baselineSeqLen : 1). */
struct KernelExponents {
    int  seq        = 0;
    int  dim        = 0;
    int  vocab      = 0;
    bool runtimeSeq = false;
};

inline KernelExponents kernelExponents(int kernelId)
{
    switch (kernelId) {
    case PATCHIFICATION_EMBED:   return {0, 2, 0, false};
    case VIS_ATTN_PROJ:          return {0, 2, 0, false};
    case GLOBAL_SPATIAL_ATTN:    return {0, 1, 0, false};
    case VIS_FFN:                return {0, 2, 0, false};
    case MLP_PROJECTOR:          return {0, 2, 0, false};
    case SEQ_CONCAT:             return {1, 0, 0, false};
    case PREFILL_ATTN_PROJ:      return {1, 2, 0, false};
    case PREFILL_CAUSAL_ATTN:    return {2, 1, 0, true};
    case PREFILL_FFN:            return {1, 2, 0, false};
    case GEMV_PROJECT:           return {0, 2, 0, false};
    case KV_CACHE_ATTN:          return {1, 1, 0, true};
    case DECODE_FFN:             return {0, 2, 0, false};
    case LM_HEAD:                return {0, 1, 1, false};
    case DETOK_DEQUANT:          return {0, 0, 1, false};
    case IDLE:
    case VISION_INGESTION:
    case FAST_IDCT:
    case ACTUATE:
    default:                     return {0, 0, 0, false};
    }
}

/* actionTokenCount/currentLayer are reserved for future per-step/per-layer heterogeneity. */
inline uint64_t computeScaledDelayPs(uint64_t basePs,
                                     int      kernelId,
                                     double   scaleSeq,
                                     double   scaleDim,
                                     double   scaleVocab,
                                     int      currentSeqLen,
                                     int      baselineSeqLen,
                                     int      /*actionTokenCount*/,
                                     int      /*currentLayer*/)
{
    if (kernelId < 0 || kernelId >= NUM_STATES) return 0;
    if (basePs == 0)                             return 0;

    KernelExponents e = kernelExponents(kernelId);

    double effSeq = scaleSeq;
    if (e.runtimeSeq && baselineSeqLen > 0 && currentSeqLen > 0) {
        effSeq *= static_cast<double>(currentSeqLen)
                / static_cast<double>(baselineSeqLen);
    }

    double factor = 1.0;
    for (int i = 0; i < e.seq;   ++i) factor *= effSeq;
    for (int i = 0; i < e.dim;   ++i) factor *= scaleDim;
    for (int i = 0; i < e.vocab; ++i) factor *= scaleVocab;

    return static_cast<uint64_t>(static_cast<double>(basePs) * factor);
}

/* Legacy scale_factor path; preserved so existing calibrations keep their meaning. */
inline uint64_t computeLegacyScaledDelayPs(uint64_t basePs,
                                           int      kernelId,
                                           double   scaleFactor)
{
    if (kernelId < 0 || kernelId >= NUM_STATES) return 0;
    if (basePs == 0)                             return 0;

    int order = complexityOrder(kernelId);
    double scaled = static_cast<double>(basePs);
    for (int i = 0; i < order; ++i) scaled *= scaleFactor;
    return static_cast<uint64_t>(scaled);
}

/* Route a kernelId to its relevant FSM layer counter; 0 for out-of-loop kernels. */
inline int pickCurrentLayer(int kernelId,
                            int vitLayer,
                            int prefillLayer,
                            int decodeLayer)
{
    switch (kernelId) {
    case PATCHIFICATION_EMBED:
    case VIS_ATTN_PROJ:
    case GLOBAL_SPATIAL_ATTN:
    case VIS_FFN:
    case MLP_PROJECTOR:
        return vitLayer;
    case PREFILL_ATTN_PROJ:
    case PREFILL_CAUSAL_ATTN:
    case PREFILL_FFN:
        return prefillLayer;
    case GEMV_PROJECT:
    case KV_CACHE_ATTN:
    case DECODE_FFN:
    case LM_HEAD:
        return decodeLayer;
    default:
        return 0;
    }
}

/** Positional CSV: token N -> out[N] (gaps stay 0, no shift). Allows NUM_STATES or NUM_STATES-1 (IDLE omitted). */
inline int parseBaselinePsCsv(const std::string& csv,
                              uint64_t out[NUM_STATES],
                              SST::Output* log,
                              const char* who)
{
    auto trim = [](std::string& s) {
        size_t b = 0;
        while (b < s.size() &&
               std::isspace(static_cast<unsigned char>(s[b]))) ++b;
        size_t e = s.size();
        while (e > b &&
               std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
        s = s.substr(b, e - b);
    };

    if (!who) who = "VLA delay agent";

    int filled = 0;
    int seenTokens = 0;
    std::istringstream ss(csv);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        ++seenTokens;
        trim(tok);

        if (filled >= NUM_STATES) {
            if (log)
                log->output("%s: baseline_ps has more than %d values "
                            "(token #%d='%s'); dropping extras.\n",
                            who, NUM_STATES, seenTokens, tok.c_str());
            continue;
        }

        if (tok.empty()) {
            if (log)
                log->output("%s: baseline_ps token #%d is empty; "
                            "leaving slot %d as 0.\n",
                            who, seenTokens, filled);
            out[filled++] = 0;
            continue;
        }

        try {
            size_t consumed = 0;
            uint64_t v = std::stoull(tok, &consumed);
            if (consumed != tok.size() && log) {
                log->output("%s: baseline_ps token #%d ('%s') has "
                            "trailing junk; using parsed prefix %"
                            PRIu64 ".\n",
                            who, seenTokens, tok.c_str(), v);
            }
            out[filled++] = v;
        } catch (const std::exception& e) {
            if (log)
                log->output("%s: baseline_ps token #%d ('%s') is not a "
                            "valid number (%s); leaving slot %d as 0.\n",
                            who, seenTokens, tok.c_str(), e.what(),
                            filled);
            out[filled++] = 0;
        }
    }

    if (filled != NUM_STATES && filled != NUM_STATES - 1 && log) {
        log->output("%s: baseline_ps parsed %d values; expected %d "
                    "(or %d if IDLE is omitted). Unset slots remain 0.\n",
                    who, filled, NUM_STATES, NUM_STATES - 1);
    }

    return filled;
}

} // namespace Carcosa
} // namespace SST

#endif /* CARCOSA_VLA_KERNEL_COMPLEXITY_H */
