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

#ifndef SST_ELEMENTS_CARCOSA_ECC_GUARD_H
#define SST_ELEMENTS_CARCOSA_ECC_GUARD_H

// Inline memHierarchy ECC boundary; classifies access outcomes under a
// configurable scheme and applies kernel-aware scrub latencies via a self-link.

#include "sst/elements/carcosa/Components/EccPolicy.h"
#include "sst/elements/carcosa/Components/EccScheme.h"
#include "sst/elements/carcosa/Components/PipelineStateRegistry.h"
#include "sst/elements/memHierarchy/memEvent.h"
#include <sst/core/component.h>
#include <sst/core/event.h>
#include <sst/core/link.h>
#include <sst/core/output.h>
#include <sst/core/rng/mersenne.h>
#include <cstdint>
#include <random>
#include <string>

namespace SST {
namespace Carcosa {

// Self-link carrier: wraps the original MemEvent + direction so the handler
// can re-emit on the correct outgoing link after the scheduled latency.
class EccGuardDelayEvent : public SST::Event {
public:
    EccGuardDelayEvent() : SST::Event(), original_(nullptr), down_(true) {}
    EccGuardDelayEvent(SST::Event* original, bool down)
        : SST::Event(), original_(original), down_(down) {}
    ~EccGuardDelayEvent() override = default;

    SST::Event* original() const { return original_; }
    bool        isDown()   const { return down_; }
    void        clearOriginal() { original_ = nullptr; }

    EccGuardDelayEvent* clone() override {
        return new EccGuardDelayEvent(original_, down_);
    }

private:
    SST::Event* original_;
    bool        down_;

    void serialize_order(SST::Core::Serialization::serializer& ser) override {
        Event::serialize_order(ser);
        SST_SER(down_);
    }
    ImplementSerializable(SST::Carcosa::EccGuardDelayEvent);
};

class EccGuard : public SST::Component {
public:
    SST_ELI_REGISTER_COMPONENT(
        EccGuard,
        "Carcosa",
        "EccGuard",
        SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "Inline ECC boundary: classifies access outcomes (clean/correctable/DUE/escape) "
        "under a configurable scheme and applies kernel-aware scrub latencies via a "
        "self-link. Reads currentKernel from PipelineStateRegistry.",
        COMPONENT_CATEGORY_MEMORY)

    SST_ELI_DOCUMENT_PARAMS(
        {"verbose",                  "Enable verbose output.", "false"},
        {"state_key",                "PipelineStateRegistry<PipelineStateBase> key whose currentKernel field is consulted to pick a per-kernel policy. Empty disables kernel-aware lookup.", ""},
        {"ecc_scheme",               "Uniform fallback ECC scheme: 'none', 'secded', or 'chipkill'.", "none"},
        {"ber",                      "Uniform fallback per-bit error rate per access.", "0.0"},
        {"correctable_latency_ps",   "Uniform fallback scrub latency (ps) for correctable outcomes.", "0"},
        {"due_latency_ps",           "Uniform fallback latency (ps) for detected-uncorrectable outcomes.", "0"},
        {"escape_latency_ps",        "Uniform fallback latency (ps) for silent-escape outcomes (typically 0).", "0"},
        {"kernel_policy",            "CSV of per-kernel overrides; entries 'KERNEL:scheme:ber:correctable_ps:due_ps:escape_ps'. Anything not listed inherits the uniform fallback.", ""},
        {"apply_on_responses_only",  "If true, only apply ECC modeling to MemEvent responses (read returns). Writes pass through.", "true"},
        {"seed",                     "RNG seed (0 = pick a default).", "0"})

    SST_ELI_DOCUMENT_PORTS(
        {"highlink", "Link toward the directory/cache side", {"memHierarchy.MemEventBase"}},
        {"lowlink",  "Link toward the memory controller side", {"memHierarchy.MemEventBase"}})

    SST_ELI_DOCUMENT_STATISTICS(
        {"events_total",          "Total events that traversed the guard.", "count", 1},
        {"events_clean",          "Events classified clean.", "count", 1},
        {"events_correctable",    "Events classified correctable.", "count", 1},
        {"events_due",            "Events classified DUE.", "count", 1},
        {"events_escape",         "Events classified silent escape.", "count", 1},
        {"latency_added_ps",      "Total ps of ECC scrub/DUE latency added.", "ps", 1})

    EccGuard(SST::ComponentId_t id, SST::Params& params);
    ~EccGuard() override;

    void setup() override;
    void init(unsigned phase) override;
    void complete(unsigned phase) override;
    void finish() override;

private:
    void handleHighlink(SST::Event* ev);
    void handleLowlink(SST::Event* ev);
    void handleSelf(SST::Event* ev);

    uint64_t applyPolicy(SST::MemHierarchy::MemEvent* mev);
    void flipRandomBit(SST::MemHierarchy::MemEvent* mev);
    void resolveStateLazy();

    SST::Output*  out_      = nullptr;
    bool          verbose_  = false;

    SST::Link*    highlink_ = nullptr;
    SST::Link*    lowlink_  = nullptr;
    SST::Link*    selfLink_ = nullptr;

    EccPolicyTable policy_;
    bool           applyOnResponsesOnly_ = true;

    std::string                state_key_;
    const PipelineStateBase*   state_ptr_ = nullptr;

    SST::RNG::MersenneRNG rng_;
    std::mt19937          stdRng_;

    Statistics::Statistic<uint64_t>* stat_total_       = nullptr;
    Statistics::Statistic<uint64_t>* stat_clean_       = nullptr;
    Statistics::Statistic<uint64_t>* stat_correctable_ = nullptr;
    Statistics::Statistic<uint64_t>* stat_due_         = nullptr;
    Statistics::Statistic<uint64_t>* stat_escape_      = nullptr;
    Statistics::Statistic<uint64_t>* stat_latency_     = nullptr;

    struct PerKernelCounters {
        uint64_t clean       = 0;
        uint64_t correctable = 0;
        uint64_t due         = 0;
        uint64_t escape      = 0;
        uint64_t latency_ps  = 0;
    };
    // Last slot is the catch-all for kernel_id < 0 (no FSM publisher yet).
    PerKernelCounters per_kernel_[NUM_STATES + 1]{};
};

} // namespace Carcosa
} // namespace SST

#endif /* SST_ELEMENTS_CARCOSA_ECC_GUARD_H */
