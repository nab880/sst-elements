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
//
// FAULT-MODEL CALIBRATION
// =======================
// fault_model='jedec_mix' samples one of {single-cell, single-word,
// single-row, single-column, single-bank, single-device} fault events per
// access. The default mixture weights (0.55:0.15:0.10:0.08:0.07:0.05) follow
// the dominant-mode breakdown reported in
//
//   Sridharan et al., "Memory Errors in Modern Systems: The Good, The Bad,
//   and The Ugly," ASPLOS 2015 (Table 4).
//
// They were cross-checked against
//
//   Schroeder, Pinheiro, Weber, "DRAM Errors in the Wild: A Large-Scale
//   Field Study," SIGMETRICS 2009.
//
// Bit-error spans per mode (kFaultModeBitsLow / kFaultModeBitsHigh) are
// intentionally conservative for SECDED_64 / CHIPKILL_x4 classification and
// can be overridden via the fault_mode_weights parameter for sensitivity
// sweeps. fit_per_mbit_per_hour translates a published-FIT calibration into
// the per-event probability EccGuard consumes; it is independent of BER and
// is reported in setup() for reviewer transparency.
//
// DUE-AS-MCE
// ==========
// due_action='drop_frame' models the production AV-stack response to a
// Detectable-Uncorrectable Error: the guard sets
// PipelineStateBase::frameAbortRequested and the VLA pipeline agents fast-
// forward to ACTUATE on the next status write, dropping the in-flight frame
// and clearing the flag. frames_aborted is exposed as a statistic and
// surfaced in the ActionScorer's per-frame trace.

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
        {"kernel_policy",            "CSV of per-kernel/per-region overrides; entries 'KERNEL:scheme:ber:c_ps:d_ps:e_ps' or 'KERNEL@REGION:...' or '*@REGION:...'. Resolution precedence: (kernel,region) > region > kernel > uniform.", ""},
        {"apply_on_responses_only",  "If true, only apply ECC modeling to MemEvent responses (read returns). Writes pass through.", "true"},
        {"fault_model",              "Per-event fault sampler: 'poisson' (per-bit Bernoulli/Poisson on payload), 'jedec_mix' (mixture of single-cell/word/row/column/bank/device events with mixture weights derived from Sridharan ASPLOS'15 Table 4 plus Schroeder SIGMETRICS'09), or 'campaign' (deterministic fault budget keyed to a target VLA kernel; see campaign_* params).", "poisson"},
        {"campaign_target_kernel",   "Campaign mode only: VLA kernel id (integer) or kernel name (e.g. 'KV_CACHE_ATTN', 'ACTUATE') into which the entire fault budget is injected. -1 / 'any' targets every access (uniform campaign).", "any"},
        {"campaign_mode",            "Campaign mode only: which fault mode to inject ('cell','word','row','column','bank','device').", "row"},
        {"campaign_event_budget",    "Campaign mode only: total number of fault events to inject across the run; once exhausted the guard reverts to clean classification on every subsequent access. 0 disables campaign injection regardless of fault_model.", "0"},
        {"campaign_event_rate",      "Campaign mode only: per-eligible-access probability of firing one campaign event. Eligible accesses are those whose currentKernel matches campaign_target_kernel.", "0.0"},
        {"campaign_max_events_per_kernel_entry", "Campaign mode only: cap fault events per contiguous visit to campaign_target_kernel (e.g. 1 per ACTUATE frame). 0 disables the per-entry cap.", "0"},
        {"campaign_errors_fixed",    "Campaign mode only: if >0, inject exactly this many bit errors per event instead of sampling from the mode's [lo,hi] span.", "0"},
        {"campaign_force_multi_chip", "Campaign mode only: when true (or campaign_mode='multi_chip'), distribute chipkill errors across at least three x4 chips.", "false"},
        {"addr_filter_region",       "If set (e.g. 'action_queue'), only inject faults on MemEvents whose virtual address overlaps that published region. Empty disables filtering.", ""},
        {"addr_filter_len",          "When addr_filter_region is set, limit injection to the first N bytes of that region (0 = entire region).", "0"},
        {"fault_mode_weights",       "JEDEC mixture weights as a CSV 'cell:word:row:column:bank:device'; need not sum to 1 (normalized internally). Defaults to '0.55:0.15:0.10:0.08:0.07:0.05'.", ""},
        {"fault_event_rate",         "When fault_model='jedec_mix', per-access probability that a correlated fault event occurs (overrides BER for the mode draw). 0.0 falls back to BER * payload_bits as the event rate (Poisson approximation).", "0.0"},
        {"payload_dtype",            "Data-type-aware flip target for the silent-escape path: 'bytes' (current behavior), 'bf16', 'fp8', 'int8'. High-blast bits (sign/high exponent) are tracked separately in escape_high_blast vs escape_low_blast.", "bytes"},
        {"due_action",               "How to model a Detectable-Uncorrectable Error: 'latency_only' (current behavior; add latency, deliver original payload), or 'drop_frame' (set PipelineStateBase::frameAbortRequested so the VLA agents jump to ACTUATE and increment frames_dropped).", "latency_only"},
        {"fit_per_mbit_per_hour",    "Optional FIT calibration: when >0 and fault_event_rate==0, the guard derives event_rate = (FIT/Mbit/h) * dram_capacity_mb * (sim_time_per_event_ns/3.6e15). Reported in setup() so reviewers see a single FIT number.", "0.0"},
        {"dram_capacity_mb",         "Companion to fit_per_mbit_per_hour. DRAM capacity in MiB used for FIT->event_rate derivation.", "1024"},
        {"sim_time_per_event_ns",    "Companion to fit_per_mbit_per_hour. Wall-clock interval in nanoseconds that one simulated MemEvent represents (e.g. average DRAM access latency).", "100"},
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
        {"latency_added_ps",      "Total ps of ECC scrub/DUE latency added.", "ps", 1},
        {"events_correlated_row", "JEDEC mix: faults landing in a single DRAM row.", "count", 1},
        {"events_correlated_bank","JEDEC mix: faults landing in a single DRAM bank.", "count", 1},
        {"events_correlated_device","JEDEC mix: faults attributable to a single device.", "count", 1},
        {"escape_high_blast",     "Silent escapes whose flipped bit hit a high-blast position (sign / high exponent).", "count", 1},
        {"escape_low_blast",      "Silent escapes whose flipped bit hit a low-blast position (mantissa LSBs).", "count", 1},
        {"frames_aborted",        "Frames the guard requested be aborted via due_action='drop_frame'.", "count", 1})

    EccGuard(SST::ComponentId_t id, SST::Params& params);
    ~EccGuard() override;

    void setup() override;
    void init(unsigned phase) override;
    void complete(unsigned phase) override;
    void finish() override;

    enum class FaultModel : uint8_t { Poisson, JedecMix, Campaign };
    enum class PayloadDtype : uint8_t { Bytes, Bf16, Fp8, Int8 };
    enum class DueAction   : uint8_t { LatencyOnly, DropFrame };

    enum class FaultMode : uint8_t {
        SingleCell    = 0,
        SingleWord    = 1,
        SingleRow     = 2,
        SingleColumn  = 3,
        SingleBank    = 4,
        SingleDevice  = 5,
        Count
    };

    // Output of one fault sample. `per_word_errors[i]` is the bit-error count
    // for the i-th ECC word on the line; `num_errors` is the sum across words
    // (kept for stats / verbose logging). For schemes with no word concept
    // (EccScheme::NONE), per_word_errors carries a single entry equal to
    // num_errors. Caller must size per_word_errors to the scheme-appropriate
    // word count BEFORE calling drawFault*, OR rely on the draw to size it.
    struct FaultDraw {
        unsigned              num_errors = 0;
        FaultMode             mode       = FaultMode::SingleCell;
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
    bool     flipDataTypeAware(SST::MemHierarchy::MemEvent* mev,
                                bool& wasHighBlast);
    void     flipRandomBit(SST::MemHierarchy::MemEvent* mev,
                            bool& wasHighBlast);
    FaultDraw drawFaultPoisson(uint32_t payload_bytes, double ber, EccScheme scheme);
    FaultDraw drawFaultJedecMix(uint32_t payload_bytes, double event_rate, EccScheme scheme);
    void      distributeErrorsToChips(std::vector<uint8_t>& chip_counts,
                                      unsigned errs, EccScheme scheme, FaultMode mode);
    // Campaign injection: deterministic budget gated on (current kernel ==
    // campaign_target_kernel_) and per-access probability
    // campaign_event_rate_; see EccGuard.h docs.
    FaultDraw drawFaultCampaign(uint32_t payload_bytes, EccScheme scheme,
                                 int kernel_id);

    // Emit a one-shot warning whenever a policy entry's BER exceeds the
    // documented tight-approximation bound (see kEccBerTightUpperBound in
    // EccScheme.h). Tracks already-warned BER values to avoid log spam.
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
    /** Campaign + addr_filter: also inject on CPU writes (payload present). */
    bool shouldApplyPolicy(SST::MemHierarchy::MemEvent* mev);
    void noteCampaignKernelEntry(int kernel_id);

    void requestFrameAbort();

    SST::Output*  out_      = nullptr;
    bool          verbose_  = false;

    SST::Link*    highlink_ = nullptr;
    SST::Link*    lowlink_  = nullptr;
    SST::Link*    selfLink_ = nullptr;

    EccPolicyTable policy_;
    bool           applyOnResponsesOnly_ = true;

    FaultModel    fault_model_   = FaultModel::Poisson;
    PayloadDtype  payload_dtype_ = PayloadDtype::Bytes;
    DueAction     due_action_    = DueAction::LatencyOnly;

    // JEDEC mixture weights, normalized in ctor; size == FaultMode::Count.
    double mode_weights_[static_cast<int>(FaultMode::Count)] = {};
    double fault_event_rate_ = 0.0;

    // Campaign-mode parameters (only consulted when fault_model_ == Campaign).
    // campaign_target_kernel_ < 0 means "any kernel". campaign_mode_ picks the
    // single FaultMode to inject. campaign_event_budget_ counts down to zero;
    // once depleted the guard returns Clean for every subsequent access.
    int       campaign_target_kernel_ = -1;
    FaultMode campaign_mode_          = FaultMode::SingleRow;
    uint64_t  campaign_event_budget_  = 0;
    double    campaign_event_rate_    = 0.0;
    uint64_t  campaign_events_fired_  = 0;
    uint64_t  campaign_max_per_kernel_entry_ = 0;
    uint64_t  campaign_events_this_entry_    = 0;
    int       campaign_entry_kernel_         = -2;
    /** When addr_filter_region_ is set, cap per pipeline frame (async ReadResp). */
    int       campaign_entry_pipeline_cycle_ = -1;
    unsigned  campaign_errors_fixed_         = 0;
    bool      campaign_force_multi_chip_     = false;

    std::string addr_filter_region_;
    uint64_t    addr_filter_len_  = 0;

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
    Statistics::Statistic<uint64_t>* stat_correlated_row_        = nullptr;
    Statistics::Statistic<uint64_t>* stat_correlated_bank_       = nullptr;
    Statistics::Statistic<uint64_t>* stat_correlated_device_     = nullptr;
    Statistics::Statistic<uint64_t>* stat_escape_high_blast_     = nullptr;
    Statistics::Statistic<uint64_t>* stat_escape_low_blast_      = nullptr;
    Statistics::Statistic<uint64_t>* stat_frames_aborted_        = nullptr;

    struct OutcomeCounters {
        uint64_t clean       = 0;
        uint64_t correctable = 0;
        uint64_t due         = 0;
        uint64_t escape      = 0;
        uint64_t latency_ps  = 0;
    };
    // Last slot is the catch-all for kernel_id < 0 (no FSM publisher yet).
    OutcomeCounters per_kernel_[NUM_STATES + 1]{};

    // (kernel_id_or_NUM_STATES, region_name) -> counters. Region "" means
    // "address didn't fall in any published region" (i.e. unlabeled DRAM).
    std::map<std::pair<int, std::string>, OutcomeCounters> per_kernel_region_;

    // Fault-mode draw counters; written every time fault_model_=JedecMix fires.
    uint64_t per_mode_draws_[static_cast<int>(FaultMode::Count)] = {};

    // Tracked by data-type-aware flipper for the run-end summary.
    uint64_t escape_high_blast_total_ = 0;
    uint64_t escape_low_blast_total_  = 0;

    uint64_t frames_aborted_total_    = 0;

    // Track which BER values have already triggered the tight-bound warning
    // (key is the bit-pattern of the double so we don't worry about == on
    // floats). Set in warnIfBerExceedsTightBound.
    std::set<uint64_t> ber_warned_;
};

} // namespace Carcosa
} // namespace SST

#endif /* SST_ELEMENTS_CARCOSA_ECC_GUARD_H */
