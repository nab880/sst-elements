// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#ifndef SST_ELEMENTS_CARCOSA_ECC_GUARD_H
#define SST_ELEMENTS_CARCOSA_ECC_GUARD_H

// Inline memHierarchy ECC boundary: classify outcomes, apply scrub latency.
// Poisson bit faults with generic kernel/region policy overrides.

#include "sst/elements/carcosa/components/eccPolicy.h"
#include "sst/elements/carcosa/components/eccPayloadCorruptor.h"
#include "sst/elements/carcosa/components/componentTestBounds.h"
#include "sst/elements/carcosa/components/eccScheme.h"
#include "sst/elements/carcosa/components/pipelineStateRegistry.h"
#include "sst/elements/memHierarchy/memEvent.h"
#include <sst/core/component.h>
#include <sst/core/event.h>
#include <sst/core/link.h>
#include <sst/core/output.h>
#include <sst/core/rng/mersenne.h>
#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

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
        SST_SER(original_);
        SST_SER(down_);
    }
    ImplementSerializable(SST::Carcosa::EccGuardDelayEvent);
};

class EccGuard : public SST::Component {
public:
    SST_ELI_REGISTER_COMPONENT(
        EccGuard,
        "carcosa",
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
        {"kernel_policy",            "CSV of per-kernel/per-region overrides; entries 'KERNEL:scheme:ber:c_ps:d_ps:e_ps' or 'KERNEL@REGION:...' or '*@REGION:...'. Resolution precedence: (kernel,region) > region > kernel > uniform.", ""},
        {"apply_on_responses_only",  "If true, only apply ECC modeling to MemEvent responses (read returns). Writes pass through.", "true"},
        {"fault_model", "Per-bit Poisson sampler; only poisson is supported by the ECC foundation.", "poisson"},
        {"addr_filter_region",       "If set (e.g. 'action_queue'), only inject faults on MemEvents whose virtual address overlaps that published region. Empty disables filtering.", ""},
        {"addr_filter_len",          "When addr_filter_region is set, limit injection to the first N bytes of that region (0 = entire region).", "0"},
        {"inject_addr_start",        "Raw injection-window base (physical/SST address). When inject_addr_len>0, inject ONLY on events overlapping [inject_addr_start, inject_addr_start+inject_addr_len). Needs no published region, unlike addr_filter_region.", "0"},
        {"inject_addr_len",          "Length in bytes of the raw injection window (see inject_addr_start). 0 disables raw-window confinement.", "0"},
        {"payload_dtype",            "Data-type-aware flip target for the silent-escape path: 'bytes' (current behavior), 'bf16', 'fp8', 'int8'. High-blast bits (sign/high exponent) are tracked separately in escape_high_blast vs escape_low_blast.", "bytes"},
        {"due_action", "Detected-uncorrectable action: latency_only adds latency and forwards poisoned data. Frame actions require the frame integration component.", "latency_only"},
        {"seed",                     "RNG seed (0 = pick a default).", "0"},
        {"test_total_min",           "Test branch hook: minimum total outcomes (-1 disables).", "-1"},
        {"test_total_max",           "Test branch hook: maximum total outcomes (-1 disables).", "-1"},
        {"test_clean_min",           "Test branch hook: minimum clean outcomes.", "-1"},
        {"test_clean_max",           "Test branch hook: maximum clean outcomes.", "-1"},
        {"test_correctable_min",     "Test branch hook: minimum correctable outcomes.", "-1"},
        {"test_correctable_max",     "Test branch hook: maximum correctable outcomes.", "-1"},
        {"test_due_min",             "Test branch hook: minimum DUE outcomes.", "-1"},
        {"test_due_max",             "Test branch hook: maximum DUE outcomes.", "-1"},
        {"test_escape_min",          "Test branch hook: minimum escape outcomes.", "-1"},
        {"test_escape_max",          "Test branch hook: maximum escape outcomes.", "-1"})

    SST_ELI_DOCUMENT_PORTS(
        {"highlink", "Link toward the directory/cache side", {"memHierarchy.MemEventBase"}},
        {"lowlink",  "Link toward the memory controller side", {"memHierarchy.MemEventBase"}})

    SST_ELI_DOCUMENT_STATISTICS(
        {"events_total",          "Total events that traversed the guard.", "count", 1},
        {"events_clean",          "Events classified clean.", "count", 1},
        {"events_correctable",    "Events classified correctable.", "count", 1},
        {"events_due",            "Events classified DUE.", "count", 1},
        {"events_escape",         "Events classified silent escape.", "count", 1},
        {"latency_added_ps",      "Total ps of ECC scrub/DUE latency added.", "ps", 1},
        {"escape_high_blast",     "Silent escapes whose flipped bit hit a high-blast position (sign / high exponent).", "count", 1},
        {"escape_low_blast",      "Silent escapes whose flipped bit hit a low-blast position (mantissa LSBs).", "count", 1},
        {"due_poisoned_bits",     "Bits flipped into forwarded payloads by DUE words under due_action='latency_only' (poison forwarding).", "count", 1})

    EccGuard(SST::ComponentId_t id, SST::Params& params);
    ~EccGuard() override;

    void setup() override;
    void init(unsigned phase) override;
    void complete(unsigned phase) override;
    void finish() override;

    enum class FaultModel : uint8_t { Poisson };
    using PayloadDtype = EccPayloadDtype;
    enum class DueAction   : uint8_t { LatencyOnly };

    // One fault sample: per_word_errors[i] = bit errors in word i; num_errors
    // is their sum. NONE schemes use a single entry. Caller sizes the vector
    // before drawFault*, or relies on the draw to resize.
    struct FaultDraw {
        unsigned              num_errors = 0;
        std::vector<unsigned> per_word_errors;
        // Per-word per-chip error counts for chip-aware chipkill
        // classification. Outer index = word, inner index = chip within word.
        // Only populated when scheme == CHIPKILL_x4.
        std::vector<std::vector<uint8_t>> per_word_chip_errors;
    };

private:
    void handleHighlink(SST::Event* ev);
    void handleLowlink(SST::Event* ev);
    void handleSelf(SST::Event* ev);

    uint64_t applyPolicy(SST::MemHierarchy::MemEvent* mev);
    FaultDraw drawFaultPoisson(uint32_t payload_bytes, double ber, EccScheme scheme);
    void distributeErrorsToChips(std::vector<uint8_t>& chip_counts,
                                 unsigned errs, EccScheme scheme);
    // Emit a one-shot warning whenever a policy entry's BER exceeds the
    // documented tight-approximation bound (see kEccBerTightUpperBound in
    // eccScheme.h). Tracks already-warned BER values to avoid log spam.
    void warnIfBerExceedsTightBound(double ber, const char* origin);

    void resolveStateLazy();
    int  resolveRegionId(uint64_t addr) const;
    // Prefer the original virtual address carried on the MemEvent (stamped by
    // the dTLB wrapper) so we match the agent-published virtual regions; fall
    // back to the physical address.
    int  resolveRegionIdForEvent(SST::MemHierarchy::MemEvent* mev) const;
    const std::string& regionNameForId(int region_id) const;
    bool resolveAddrFilterBounds(uint64_t& base_out, uint64_t& len_out) const;
    bool eventOverlapsAddrFilter(SST::MemHierarchy::MemEvent* mev) const;
    bool shouldApplyPolicy(SST::MemHierarchy::MemEvent* mev);


    SST::Output*  out_      = nullptr;
    bool          verbose_  = false;

    SST::Link*    highlink_ = nullptr;
    SST::Link*    lowlink_  = nullptr;
    SST::Link*    selfLink_ = nullptr;

    EccPolicyTable policy_;
    bool           apply_on_responses_only_ = true;

    FaultModel    fault_model_   = FaultModel::Poisson;
    PayloadDtype  payload_dtype_ = PayloadDtype::Bytes;
    DueAction     due_action_    = DueAction::LatencyOnly;

    std::string addr_filter_region_;
    uint64_t    addr_filter_len_  = 0;
    // Raw inject window: when inject_addr_len_ > 0, only events overlapping
    // [inject_addr_start_, +len). For transports with no region registry.
    uint64_t    inject_addr_start_ = 0;
    uint64_t    inject_addr_len_   = 0;

    std::string                state_key_;
    const PipelineStateBase*   state_ptr_ = nullptr;

    SST::RNG::MersenneRNG rng_;
    std::mt19937          stdRng_;

    Statistics::Statistic<uint64_t>* stat_total_                 = nullptr;
    Statistics::Statistic<uint64_t>* stat_clean_                 = nullptr;
    Statistics::Statistic<uint64_t>* stat_correctable_           = nullptr;
    Statistics::Statistic<uint64_t>* stat_due_                   = nullptr;
    Statistics::Statistic<uint64_t>* stat_escape_                = nullptr;
    Statistics::Statistic<uint64_t>* stat_latency_               = nullptr;
    Statistics::Statistic<uint64_t>* stat_escape_high_blast_     = nullptr;
    Statistics::Statistic<uint64_t>* stat_escape_low_blast_      = nullptr;
    Statistics::Statistic<uint64_t>* stat_due_poisoned_          = nullptr;

    struct OutcomeCounters {
        uint64_t clean       = 0;
        uint64_t correctable = 0;
        uint64_t due         = 0;
        uint64_t escape      = 0;
        uint64_t latency_ps  = 0;
    };
    // Per-kernel counters keyed by the workload-supplied kernel name. The
    // empty string is the catch-all for "no FSM publisher / unknown kernel".
    std::map<std::string, OutcomeCounters> per_kernel_;

    // (kernel_name, region_name) -> counters. Region "" means "address
    // didn't fall in any published region" (i.e. unlabeled DRAM); kernel
    // "" means "no FSM publisher yet".
    std::map<std::pair<std::string, std::string>, OutcomeCounters> per_kernel_region_;

    ComponentTestBounds test_bounds_;

    // Tracked by data-type-aware flipper for the run-end summary.
    uint64_t escape_high_blast_total_ = 0;
    uint64_t escape_low_blast_total_  = 0;

    // Bits flipped into forwarded payloads by latency_only DUE poisoning.
    uint64_t due_poison_flips_total_  = 0;


    // Track which BER values have already triggered the tight-bound warning
    // (key is the bit-pattern of the double so we don't worry about == on
    // floats). Set in warnIfBerExceedsTightBound.
    std::set<uint64_t> ber_warned_;
};

} // namespace Carcosa
} // namespace SST

#endif /* SST_ELEMENTS_CARCOSA_ECC_GUARD_H */
