// Copyright 2009-2025 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2025, NTESS
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#include "sst/elements/carcosa/injectors/portModuleMitigatedGate.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

using namespace SST::Carcosa;

namespace {

std::string toLower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

} // namespace

PortModuleMitigatedGate::Scheme
PortModuleMitigatedGate::parseScheme(const std::string& s)
{
    const std::string v = toLower(s);
    if (v == "off" || v.empty())       return Scheme::Off;
    if (v == "secded")                 return Scheme::Secded;
    if (v == "dmr")                    return Scheme::Dmr;
    if (v == "checkpoint")             return Scheme::Checkpoint;
    if (v == "selective_secded" ||
        v == "selectivesecded")        return Scheme::SelectiveSecded;
    return Scheme::Off;
}

PortModuleMitigatedGate::PortModuleMitigatedGate(Params& params)
    : PortModuleStateGate(params)
{
    scheme_                   = parseScheme(params.find<std::string>("mitigation_scheme", "off"));
    maskProbability_          = params.find<double>("mask_probability", -1.0);
    eccCheckCycles_           = params.find<uint64_t>("ecc_check_cycles", 2);
    dmrComputeCycles_         = params.find<uint64_t>("dmr_compute_cycles", 100);
    checkpointRollbackCycles_ = params.find<uint64_t>("checkpoint_rollback_cycles", 10000);

#ifdef __SST_DEBUG_OUTPUT__
    dbg_->debug(CALL_INFO_LONG, 1, 0,
                "PortModuleMitigatedGate: scheme=%d mask_p=%f ecc_c=%lu "
                "dmr_c=%lu ckpt_c=%lu\n",
                static_cast<int>(scheme_), schemeDefaultMaskProb(),
                static_cast<unsigned long>(eccCheckCycles_),
                static_cast<unsigned long>(dmrComputeCycles_),
                static_cast<unsigned long>(checkpointRollbackCycles_));
#endif
}

PortModuleMitigatedGate::~PortModuleMitigatedGate() = default;

double
PortModuleMitigatedGate::schemeDefaultMaskProb() const
{
    if (maskProbability_ >= 0.0) return maskProbability_;
    switch (scheme_) {
        case Scheme::Off:              return 0.0;
        case Scheme::Secded:           return 1.0;
        case Scheme::Dmr:              return 0.999;
        case Scheme::Checkpoint:       return 0.0;
        case Scheme::SelectiveSecded:  return 1.0;
    }
    return 0.0;
}

uint64_t
PortModuleMitigatedGate::schemeCyclesPerEvent() const
{
    switch (scheme_) {
        case Scheme::Off:              return 0;
        case Scheme::Secded:           return eccCheckCycles_;
        case Scheme::SelectiveSecded:  return eccCheckCycles_;
        case Scheme::Dmr:              return dmrComputeCycles_;
        case Scheme::Checkpoint:       return checkpointRollbackCycles_;
    }
    return 0;
}

bool
PortModuleMitigatedGate::doInjection()
{
    const bool parent_triggered = PortModuleStateGate::doInjection();
    if (scheme_ == Scheme::Off) {
        // No mitigation; defer entirely to parent.
        return parent_triggered;
    }
    if (!parent_triggered) {
        // No fault to mitigate on this event. selective_secded intentionally
        // accounts for nothing here either: if the parent gate didn't match,
        // the event was outside the protected region/phase and sees no
        // overhead. This is the whole point of selective ECC.
        return false;
    }

    ++events_matched_;
    cycles_overhead_ += schemeCyclesPerEvent();

    const double mask_p = schemeDefaultMaskProb();
    const bool mask = this->randFloat(0.0, 1.0) <= mask_p;
    if (mask) {
        ++events_masked_;
        // Clear trigger flags so executeFaults() becomes a no-op.
        triggered_ = {{false, false}};
    }

    // PortModule destructors run after SST has torn down its output streams
    // on macOS (and sometimes Linux), so a dtor-time FINAL line gets lost.
    // Instead emit a running cumulative snapshot on every matched event;
    // the campaign driver scrapes the last line of the log.
    std::fprintf(stderr,
        "carcosa.PortModuleMitigatedGate STATE "
        "scheme=%d matched=%lu masked=%lu cycles_overhead=%lu\n",
        static_cast<int>(scheme_),
        static_cast<unsigned long>(events_matched_),
        static_cast<unsigned long>(events_masked_),
        static_cast<unsigned long>(cycles_overhead_));

    return !mask;
}
