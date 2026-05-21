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

#include "sst_config.h"
#include "sst/elements/carcosa/Components/EccGuard.h"
#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <numeric>
#include <sstream>
#include <vector>

using namespace SST;
using namespace SST::MemHierarchy;
using namespace SST::Carcosa;

namespace {

constexpr int kModeCount = static_cast<int>(EccGuard::FaultMode::Count);

// Sridharan ASPLOS'15 Table 4 + Schroeder SIGMETRICS'09 dominant-mode mix.
// Order: cell, word, row, column, bank, device.
constexpr double kDefaultModeWeights[kModeCount] = {
    0.55, 0.15, 0.10, 0.08, 0.07, 0.05
};

// Approximate # of bit errors a fault of each mode delivers across the line.
// For correlated modes (Single{Word,Row,Column,Bank,Device}) the entire
// per-event error count is deposited into one randomly-chosen ECC word (with
// spillover into neighbouring words when the count exceeds the word width);
// SingleCell scatters bit-by-bit. See drawFaultJedecMix for the deposit
// policy and classifyEccWord/aggregateLineOutcome (EccScheme.h) for how the
// per-word outcomes roll up. Default spans are conservative under SECDED_64
// / CHIPKILL_x4 and can be overridden via fault_mode_weights for sensitivity
// sweeps.
constexpr unsigned kFaultModeBitsLow[kModeCount]  = { 1, 2, 4, 4, 8, 32 };
constexpr unsigned kFaultModeBitsHigh[kModeCount] = { 1, 2, 8, 8, 16, 64 };

const char* faultModeName(EccGuard::FaultMode m) {
    switch (m) {
        case EccGuard::FaultMode::SingleCell:    return "single_cell";
        case EccGuard::FaultMode::SingleWord:    return "single_word";
        case EccGuard::FaultMode::SingleRow:     return "single_row";
        case EccGuard::FaultMode::SingleColumn:  return "single_column";
        case EccGuard::FaultMode::SingleBank:    return "single_bank";
        case EccGuard::FaultMode::SingleDevice:  return "single_device";
        default:                                  return "unknown";
    }
}

bool parseModeWeightsCsv(const std::string& csv, double out[kModeCount]) {
    std::stringstream ss(csv);
    std::string tok;
    int idx = 0;
    while (std::getline(ss, tok, ':')) {
        if (idx >= kModeCount) return false;
        try {
            out[idx++] = std::stod(tok);
        } catch (...) {
            return false;
        }
    }
    return idx == kModeCount;
}

EccGuard::FaultModel parseFaultModel(const std::string& s) {
    if (s == "jedec_mix" || s == "jedec" || s == "JEDEC_MIX") return EccGuard::FaultModel::JedecMix;
    if (s == "campaign"  || s == "CAMPAIGN")                  return EccGuard::FaultModel::Campaign;
    return EccGuard::FaultModel::Poisson;
}

// Parse a campaign mode name into a FaultMode. Falls back to SingleRow on
// unknown input -- we only crash on truly empty/garbage values upstream so
// that the campaign pipeline keeps the same defaulting behaviour as the
// jedec_mix mixture defaults.
EccGuard::FaultMode parseCampaignMode(const std::string& s) {
    if (s == "cell"   || s == "single_cell"   || s == "SingleCell")   return EccGuard::FaultMode::SingleCell;
    if (s == "word"   || s == "single_word"   || s == "SingleWord")   return EccGuard::FaultMode::SingleWord;
    if (s == "row"    || s == "single_row"    || s == "SingleRow")    return EccGuard::FaultMode::SingleRow;
    if (s == "column" || s == "single_column" || s == "SingleColumn") return EccGuard::FaultMode::SingleColumn;
    if (s == "bank"   || s == "single_bank"   || s == "SingleBank")   return EccGuard::FaultMode::SingleBank;
    if (s == "device" || s == "single_device" || s == "SingleDevice") return EccGuard::FaultMode::SingleDevice;
    if (s == "multi_chip" || s == "MULTI_CHIP") return EccGuard::FaultMode::SingleWord;
    return EccGuard::FaultMode::SingleRow;
}

bool isMultiChipCampaignAlias(const std::string& s) {
    return s == "multi_chip" || s == "MULTI_CHIP";
}

// Resolve a campaign_target_kernel string. Accepts either a small integer
// Resolve a campaign_target_kernel string to the canonical kernel-name key
// used by PipelineStateBase::currentKernelName. The empty result represents
// "every kernel" (uniform campaign), matching the empty currentKernelName
// the agent publishes between kernels.
std::string resolveCampaignKernel(const std::string& raw) {
    if (raw.empty() || raw == "any" || raw == "ANY" || raw == "*"
        || raw == "-1") {
        return std::string();
    }
    return raw;
}

EccGuard::PayloadDtype parseDtype(const std::string& s) {
    if (s == "bf16" || s == "BF16") return EccGuard::PayloadDtype::Bf16;
    if (s == "fp8"  || s == "FP8")  return EccGuard::PayloadDtype::Fp8;
    if (s == "int8" || s == "INT8") return EccGuard::PayloadDtype::Int8;
    return EccGuard::PayloadDtype::Bytes;
}

EccGuard::DueAction parseDueAction(const std::string& s) {
    if (s == "drop_frame" || s == "drop" || s == "DROP_FRAME") return EccGuard::DueAction::DropFrame;
    return EccGuard::DueAction::LatencyOnly;
}

const char* dtypeName(EccGuard::PayloadDtype d) {
    switch (d) {
    case EccGuard::PayloadDtype::Bytes: return "bytes";
    case EccGuard::PayloadDtype::Bf16:  return "bf16";
    case EccGuard::PayloadDtype::Fp8:   return "fp8";
    case EccGuard::PayloadDtype::Int8:  return "int8";
    }
    return "unknown";
}

// Returns true if the given bit (0-indexed inside its element) is "high blast"
// for the dtype (sign bit or top exponent bit). Bit 0 is LSB of the element.
bool isHighBlastBit(EccGuard::PayloadDtype dtype, unsigned bit_in_element) {
    switch (dtype) {
    case EccGuard::PayloadDtype::Bf16: {
        // bf16: [15] sign, [14:7] exponent, [6:0] mantissa.
        if (bit_in_element == 15)        return true;       // sign
        if (bit_in_element >= 13 && bit_in_element <= 14) return true; // top 2 exp bits
        return false;
    }
    case EccGuard::PayloadDtype::Fp8: {
        // E4M3-style fp8: [7] sign, [6:3] exponent, [2:0] mantissa.
        if (bit_in_element == 7)         return true;
        if (bit_in_element == 6)         return true;
        return false;
    }
    case EccGuard::PayloadDtype::Int8: {
        // Two's-complement int8: bit 7 sign.
        return bit_in_element == 7;
    }
    case EccGuard::PayloadDtype::Bytes:
    default:
        return false;
    }
}

unsigned dtypeBytes(EccGuard::PayloadDtype d) {
    switch (d) {
    case EccGuard::PayloadDtype::Bf16:  return 2;
    case EccGuard::PayloadDtype::Fp8:
    case EccGuard::PayloadDtype::Int8:  return 1;
    case EccGuard::PayloadDtype::Bytes:
    default:                             return 1;
    }
}

} // namespace

EccGuard::EccGuard(ComponentId_t id, Params& params) : Component(id) {
    requireLibrary("memHierarchy");

    out_ = new Output("", 1, 0, Output::STDOUT);
    verbose_ = params.find<bool>("verbose", false);

    state_key_            = params.find<std::string>("state_key", "");
    applyOnResponsesOnly_ = params.find<bool>("apply_on_responses_only", true);

    EccPolicyEntry uniform;
    uniform.inherits_uniform = false;
    std::string scheme_str = params.find<std::string>("ecc_scheme", "none");
    if (!eccSchemeFromString(scheme_str, uniform.scheme)) {
        out_->fatal(CALL_INFO, -1,
            "EccGuard: unknown ecc_scheme '%s'. Use 'none', 'secded', or 'chipkill'.\n",
            scheme_str.c_str());
    }
    uniform.ber                    = params.find<double>("ber", 0.0);
    uniform.correctable_latency_ps = params.find<uint64_t>("correctable_latency_ps", 0);
    uniform.due_latency_ps         = params.find<uint64_t>("due_latency_ps",         0);
    uniform.escape_latency_ps      = params.find<uint64_t>("escape_latency_ps",      0);
    if (uniform.ber < 0.0 || uniform.ber > 1.0) {
        out_->fatal(CALL_INFO, -1,
            "EccGuard: ber=%g out of range [0.0, 1.0].\n", uniform.ber);
    }
    policy_.setUniform(uniform);

    std::string ks_csv = params.find<std::string>("kernel_policy", "");
    if (!ks_csv.empty()) {
        std::vector<std::string> errors;
        int parsed = policy_.parseCsv(ks_csv, errors);
        for (auto& e : errors) out_->output("EccGuard: %s\n", e.c_str());
        if (verbose_) {
            out_->output("EccGuard: parsed %d kernel/region policy override(s).\n", parsed);
        }
    }

    // Phase 2: fault model + dtype-aware flips + DUE action.
    fault_model_   = parseFaultModel(params.find<std::string>("fault_model",   "poisson"));
    payload_dtype_ = parseDtype     (params.find<std::string>("payload_dtype", "bytes"));
    due_action_    = parseDueAction (params.find<std::string>("due_action",    "latency_only"));

    std::string mw_csv = params.find<std::string>("fault_mode_weights", "");
    if (!mw_csv.empty()) {
        if (!parseModeWeightsCsv(mw_csv, mode_weights_)) {
            out_->fatal(CALL_INFO, -1,
                "EccGuard: malformed fault_mode_weights '%s'; need 6 colon-separated doubles "
                "(cell:word:row:column:bank:device).\n", mw_csv.c_str());
        }
    } else {
        for (int i = 0; i < kModeCount; ++i) mode_weights_[i] = kDefaultModeWeights[i];
    }
    // Normalize. (parseCsv lets users pass arbitrary positive numbers.)
    {
        double sum = 0.0;
        for (int i = 0; i < kModeCount; ++i) {
            if (mode_weights_[i] < 0.0) mode_weights_[i] = 0.0;
            sum += mode_weights_[i];
        }
        if (sum <= 0.0) {
            out_->fatal(CALL_INFO, -1,
                "EccGuard: fault_mode_weights sum to zero; provide at least one positive weight.\n");
        }
        for (int i = 0; i < kModeCount; ++i) mode_weights_[i] /= sum;
    }

    fault_event_rate_ = params.find<double>("fault_event_rate", 0.0);

    // Campaign-mode parameters. These are inert unless fault_model_ == Campaign.
    {
        std::string raw_target = params.find<std::string>("campaign_target_kernel", "any");
        campaign_target_kernel_name_ = resolveCampaignKernel(raw_target);
        if (campaign_target_kernel_name_.empty()
            && raw_target != "any" && raw_target != "ANY"
            && raw_target != "-1" && !raw_target.empty()) {
            out_->output("EccGuard WARNING: campaign_target_kernel='%s' did not "
                          "resolve to a known kernel; defaulting to 'any'.\n",
                          raw_target.c_str());
        }
        campaign_event_budget_  = params.find<uint64_t>("campaign_event_budget", 0);
        campaign_event_rate_    = params.find<double>  ("campaign_event_rate",   0.0);
        campaign_max_per_kernel_entry_ =
            params.find<uint64_t>("campaign_max_events_per_kernel_entry", 0);
        campaign_errors_fixed_ = params.find<unsigned>("campaign_errors_fixed", 0);
        std::string raw_cmode = params.find<std::string>("campaign_mode", "row");
        campaign_force_multi_chip_ =
            params.find<bool>("campaign_force_multi_chip", false)
            || isMultiChipCampaignAlias(raw_cmode);
        campaign_mode_ = parseCampaignMode(raw_cmode);
        addr_filter_region_ = params.find<std::string>("addr_filter_region", "");
        addr_filter_len_    = params.find<uint64_t>("addr_filter_len", 0);
        if (fault_model_ == FaultModel::Campaign && verbose_) {
            out_->output("EccGuard: campaign mode active: target=%s mode=%d budget=%" PRIu64
                          " rate=%.3e\n",
                          campaign_target_kernel_name_.empty()
                              ? "any"
                              : campaign_target_kernel_name_.c_str(),
                          static_cast<int>(campaign_mode_),
                          campaign_event_budget_,
                          campaign_event_rate_);
        }
        if (fault_model_ == FaultModel::Campaign && campaign_event_budget_ == 0) {
            out_->output("EccGuard WARNING: fault_model='campaign' but "
                          "campaign_event_budget==0; no faults will be injected.\n");
        }
    }
    double fit_rate     = params.find<double>("fit_per_mbit_per_hour", 0.0);
    double dram_mb      = params.find<double>("dram_capacity_mb", 1024.0);
    double per_event_ns = params.find<double>("sim_time_per_event_ns", 100.0);
    if (fit_rate > 0.0 && fault_event_rate_ <= 0.0) {
        // FIT = failures per 1e9 device-hours, per Mbit. Convert to per-event prob.
        double events_per_hour      = 3.6e12 / per_event_ns; // 3.6e12 ns/hr / ns_per_event
        double total_failures_per_h = fit_rate * 1e-9 * dram_mb;
        fault_event_rate_           = total_failures_per_h / events_per_hour;
        if (fault_event_rate_ > 1.0) fault_event_rate_ = 1.0;
        if (verbose_) {
            out_->output("EccGuard: FIT=%.3g per Mbit/h x dram_mb=%.0f, sim_ns/event=%.1f -> fault_event_rate=%.3e\n",
                         fit_rate, dram_mb, per_event_ns, fault_event_rate_);
        }
    }

    // Mersenne for the bit-pick (matches RandomFlipFault); std::mt19937 for Poisson.
    uint64_t seed = params.find<uint64_t>("seed", 0);
    if (seed != 0) {
        rng_.seed(seed);
        stdRng_.seed(static_cast<uint32_t>(seed));
    } else {
        stdRng_.seed(0xC0FFEEu);
    }

    if (isPortConnected("highlink")) {
        highlink_ = configureLink("highlink",
            new Event::Handler<EccGuard, &EccGuard::handleHighlink>(this));
    }
    if (isPortConnected("lowlink")) {
        lowlink_ = configureLink("lowlink",
            new Event::Handler<EccGuard, &EccGuard::handleLowlink>(this));
    }
    if (!highlink_ || !lowlink_) {
        out_->fatal(CALL_INFO, -1,
            "EccGuard '%s': both highlink and lowlink must be connected.\n",
            getName().c_str());
    }

    selfLink_ = configureSelfLink("ecc_self", "1ps",
        new Event::Handler<EccGuard, &EccGuard::handleSelf>(this));

    stat_total_              = registerStatistic<uint64_t>("events_total");
    stat_clean_              = registerStatistic<uint64_t>("events_clean");
    stat_correctable_        = registerStatistic<uint64_t>("events_correctable");
    stat_due_                = registerStatistic<uint64_t>("events_due");
    stat_escape_             = registerStatistic<uint64_t>("events_escape");
    stat_latency_            = registerStatistic<uint64_t>("latency_added_ps");
    stat_correlated_row_     = registerStatistic<uint64_t>("events_correlated_row");
    stat_correlated_bank_    = registerStatistic<uint64_t>("events_correlated_bank");
    stat_correlated_device_  = registerStatistic<uint64_t>("events_correlated_device");
    stat_escape_high_blast_  = registerStatistic<uint64_t>("escape_high_blast");
    stat_escape_low_blast_   = registerStatistic<uint64_t>("escape_low_blast");
    stat_frames_aborted_     = registerStatistic<uint64_t>("frames_aborted");
}

EccGuard::~EccGuard() {
    delete out_;
}

void EccGuard::init(unsigned phase) {
    if (highlink_ && lowlink_) {
        SST::Event* ev;
        while ((ev = highlink_->recvUntimedData()) != nullptr) {
            lowlink_->sendUntimedData(ev);
        }
        while ((ev = lowlink_->recvUntimedData()) != nullptr) {
            highlink_->sendUntimedData(ev);
        }
    }
    (void)phase;
}

void EccGuard::setup() {
    resolveStateLazy();

    // Upfront audit: warn for every policy entry whose BER exceeds the
    // documented tight-approximation bound (EccScheme.h::kEccBerTightUpperBound).
    // The per-event path also memoizes warnings, but firing them in setup()
    // surfaces the bound to reviewers before the simulation starts producing
    // numbers.
    policy_.forEachEntry([&](const std::string& origin, const EccPolicyEntry& e) {
        if (e.ber > 0.0) {
            warnIfBerExceedsTightBound(e.ber, origin.c_str());
        }
    });

    if (verbose_) {
        out_->output("EccGuard '%s': setup. uniform scheme=%s ber=%g state_key='%s' state_ptr=%p "
                     "fault_model=%s payload_dtype=%s due_action=%s event_rate=%.3e\n",
                     getName().c_str(),
                     eccSchemeName(policy_.uniform().scheme),
                     policy_.uniform().ber,
                     state_key_.c_str(),
                     (const void*)state_ptr_,
                     (fault_model_ == FaultModel::JedecMix
                          ? "jedec_mix"
                          : (fault_model_ == FaultModel::Campaign ? "campaign"
                                                                  : "poisson")),
                     dtypeName(payload_dtype_),
                     due_action_ == DueAction::DropFrame ? "drop_frame" : "latency_only",
                     fault_event_rate_);
    }
}

void EccGuard::complete(unsigned phase) {
    if (highlink_ && lowlink_) {
        SST::Event* ev;
        while ((ev = highlink_->recvUntimedData()) != nullptr) {
            lowlink_->sendUntimedData(ev);
        }
        while ((ev = lowlink_->recvUntimedData()) != nullptr) {
            highlink_->sendUntimedData(ev);
        }
    }
    (void)phase;
}

void EccGuard::finish() {
    out_->output("\n=== EccGuard %s Per-Kernel Outcomes ===\n", getName().c_str());
    out_->output("kernel_name,clean,correctable,due,escape,latency_ps\n");
    for (const auto& kv : per_kernel_) {
        const auto& c = kv.second;
        if (c.clean + c.correctable + c.due + c.escape == 0) continue;
        const std::string& kname = kv.first.empty() ? std::string("UNKNOWN") : kv.first;
        out_->output("%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                     kname.c_str(),
                     c.clean, c.correctable, c.due, c.escape, c.latency_ps);
    }
    out_->output("=== End EccGuard %s Per-Kernel Outcomes ===\n\n", getName().c_str());

    if (!per_kernel_region_.empty()) {
        out_->output("\n=== EccGuard %s Per-Kernel-Per-Region Outcomes ===\n", getName().c_str());
        out_->output("kernel_name,region,clean,correctable,due,escape,latency_ps\n");
        for (auto& kv : per_kernel_region_) {
            const std::string& kname  = kv.first.first.empty()  ? std::string("UNKNOWN")   : kv.first.first;
            const std::string& region = kv.first.second.empty() ? std::string("unlabeled") : kv.first.second;
            const auto& c = kv.second;
            out_->output("%s,%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                         kname.c_str(), region.c_str(),
                         c.clean, c.correctable, c.due, c.escape, c.latency_ps);
        }
        out_->output("=== End EccGuard %s Per-Kernel-Per-Region Outcomes ===\n\n", getName().c_str());
    }

    bool any_mode = false;
    for (int i = 0; i < kModeCount; ++i) if (per_mode_draws_[i] > 0) { any_mode = true; break; }
    if (any_mode) {
        out_->output("\n=== EccGuard %s Fault-Mode Draws ===\n", getName().c_str());
        out_->output("mode,count\n");
        for (int i = 0; i < kModeCount; ++i) {
            out_->output("%s,%" PRIu64 "\n",
                         faultModeName(static_cast<FaultMode>(i)), per_mode_draws_[i]);
        }
        out_->output("=== End EccGuard %s Fault-Mode Draws ===\n\n", getName().c_str());
    }

    if (escape_high_blast_total_ + escape_low_blast_total_ > 0 || frames_aborted_total_ > 0) {
        out_->output("\n=== EccGuard %s Escape/Abort Summary ===\n", getName().c_str());
        out_->output("escape_high_blast,escape_low_blast,frames_aborted,payload_dtype\n");
        out_->output("%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%s\n",
                     escape_high_blast_total_, escape_low_blast_total_,
                     frames_aborted_total_, dtypeName(payload_dtype_));
        out_->output("=== End EccGuard %s Escape/Abort Summary ===\n\n", getName().c_str());
    }
}

void EccGuard::resolveStateLazy() {
    if (state_ptr_ || state_key_.empty()) return;
    state_ptr_ = PipelineStateRegistry<PipelineStateBase>::get(state_key_);
}

int EccGuard::resolveRegionId(uint64_t addr) const {
    if (!state_ptr_) return -1;
    return state_ptr_->regionIdForAddress(addr);
}

// Region-attribution helper. EccGuard sits below the dTLB and therefore sees
// physical addresses on `mev->getAddr()`. The InterceptionAgent (Hali) sits
// above the TLB and publishes regions in *virtual* address space (that is the
// only address space the agent can see and the binary advertises). The dTLB
// wrapper stamps `vAddr_` on every translation, and MemEvent::clone() and
// makeResponse() preserve it, so by the time the event reaches EccGuard the
// originating virtual address is still available. Prefer it; fall back to the
// physical address for events that didn't originate from the LSQ (e.g. cache
// writebacks/evictions with vAddr=0).
int EccGuard::resolveRegionIdForEvent(MemEvent* mev) const {
    if (!mev || !state_ptr_) return -1;
    uint64_t vaddr = mev->getVirtualAddress();
    if (vaddr != 0) {
        int rid = state_ptr_->regionIdForAddress(vaddr);
        if (rid >= 0) return rid;
    }
    return state_ptr_->regionIdForAddress(mev->getAddr());
}

const std::string& EccGuard::regionNameForId(int region_id) const {
    static const std::string empty;
    if (!state_ptr_ || region_id < 0) return empty;
    if (region_id >= static_cast<int>(state_ptr_->regions.size())) return empty;
    return state_ptr_->regions[region_id].name;
}

bool EccGuard::resolveAddrFilterBounds(uint64_t& base_out, uint64_t& len_out) const {
    base_out = 0;
    len_out  = 0;
    if (addr_filter_region_.empty()) return false;
    if (!state_ptr_) return false;
    for (const auto& r : state_ptr_->regions) {
        if (!r.valid || r.name != addr_filter_region_) continue;
        base_out = r.base;
        len_out  = r.size;
        if (addr_filter_len_ > 0 && addr_filter_len_ < len_out)
            len_out = addr_filter_len_;
        return len_out > 0;
    }
    return false;
}

bool EccGuard::shouldApplyPolicy(MemEvent* mev) {
    if (!mev) return false;
    resolveStateLazy();
    if (!applyOnResponsesOnly_) return true;
    if (mev->isResponse()) return true;
    if (fault_model_ != FaultModel::Campaign || addr_filter_region_.empty())
        return false;
    return eventOverlapsAddrFilter(mev) && !mev->getPayload().empty();
}

bool EccGuard::eventOverlapsAddrFilter(MemEvent* mev) const {
    if (addr_filter_region_.empty() || !mev) return true;
    if (state_ptr_) {
        int rid = resolveRegionIdForEvent(mev);
        if (rid >= 0 && regionNameForId(rid) == addr_filter_region_) return true;
    }
    uint64_t fbase = 0, flen = 0;
    if (!resolveAddrFilterBounds(fbase, flen)) return false;
    uint64_t vaddr = mev->getVirtualAddress();
    uint64_t addr  = (vaddr != 0) ? vaddr : mev->getAddr();
    uint64_t size  = mev->getPayload().empty() ? 64u : mev->getPayload().size();
    uint64_t end   = addr + size;
    uint64_t fend  = fbase + flen;
    return addr < fend && end > fbase;
}

void EccGuard::noteCampaignKernelEntry(const std::string& kernel_name) {
    if (campaign_max_per_kernel_entry_ == 0) return;
    const std::string& track = (!campaign_target_kernel_name_.empty())
                                   ? campaign_target_kernel_name_
                                   : kernel_name;
    if (kernel_name != track) return;
    if (kernel_name != campaign_entry_kernel_name_) {
        campaign_entry_kernel_name_  = kernel_name;
        campaign_events_this_entry_  = 0;
    }
}

void EccGuard::requestFrameAbort() {
    if (state_key_.empty()) return;
    PipelineStateBase* s =
        PipelineStateRegistry<PipelineStateBase>::getMutable(state_key_);
    if (!s) return;
    s->frameAbortRequested = true;
    ++frames_aborted_total_;
    if (stat_frames_aborted_) stat_frames_aborted_->addData(1);
}

namespace {
// Helper: bump the registry's cumulative ECC counters so the ActionScorer
// (and any other consumer) can compute per-frame deltas. Cheap pointer chase.
void publishCumulative(const std::string& state_key, uint64_t escapes_inc,
                       uint64_t flips_inc) {
    if (state_key.empty()) return;
    PipelineStateBase* s =
        PipelineStateRegistry<PipelineStateBase>::getMutable(state_key);
    if (!s) return;
    s->eccCumulativeEscapes += escapes_inc;
    s->eccCumulativeFlips   += flips_inc;
}

// Helper: bump the per-frame per-kernel escape count consumed by Tier B
// Fig. 3a (the pipeline agent argmaxes this map at frame close to stamp
// FrameRecord.attributingKernelName, then resets the map). The map is
// keyed by the workload-supplied kernel name; the empty string is the
// catch-all for "no FSM publisher / unknown kernel".
void publishPerFrameEscape(const std::string& state_key,
                           const std::string& kernel_name) {
    if (state_key.empty()) return;
    PipelineStateBase* s =
        PipelineStateRegistry<PipelineStateBase>::getMutable(state_key);
    if (!s) return;
    s->eccPerFrameEscapesByKernel[kernel_name] += 1;
}
} // namespace

void EccGuard::handleHighlink(SST::Event* ev) {
    auto* mev = dynamic_cast<MemEvent*>(ev);
    if (!mev || !shouldApplyPolicy(mev)) {
        if (lowlink_) lowlink_->send(ev); else delete ev;
        return;
    }

    uint64_t latency_ps = applyPolicy(mev);
    if (latency_ps == 0) {
        lowlink_->send(ev);
    } else {
        selfLink_->send(static_cast<SimTime_t>(latency_ps),
                        new EccGuardDelayEvent(ev, /*down=*/true));
    }
}

void EccGuard::handleLowlink(SST::Event* ev) {
    auto* mev = dynamic_cast<MemEvent*>(ev);
    if (!mev) {
        if (highlink_) highlink_->send(ev); else delete ev;
        return;
    }
    if (!shouldApplyPolicy(mev)) {
        highlink_->send(ev);
        return;
    }

    uint64_t latency_ps = applyPolicy(mev);
    if (latency_ps == 0) {
        highlink_->send(ev);
    } else {
        selfLink_->send(static_cast<SimTime_t>(latency_ps),
                        new EccGuardDelayEvent(ev, /*down=*/false));
    }
}

void EccGuard::handleSelf(SST::Event* ev) {
    auto* pe = dynamic_cast<EccGuardDelayEvent*>(ev);
    if (!pe) { delete ev; return; }
    SST::Event* original = pe->original();
    bool        down     = pe->isDown();
    pe->clearOriginal();
    delete pe;

    if (down) {
        if (lowlink_)  lowlink_->send(original);
        else           delete original;
    } else {
        if (highlink_) highlink_->send(original);
        else           delete original;
    }
}

namespace {

// Number of ECC protection words a `payload_bytes` line contains under
// `scheme`. Falls back to 1 (treat the whole payload as one "word") when the
// scheme has no word concept (e.g. NONE).
inline uint32_t numWords(uint32_t payload_bytes, EccScheme scheme) {
    uint32_t wb = eccWordBytes(scheme);
    if (wb == 0 || payload_bytes == 0) return payload_bytes > 0 ? 1u : 0u;
    return (payload_bytes + wb - 1) / wb;
}

// Bits per ECC word for the draw. For schemes with no word concept we use
// payload bits.
inline uint32_t bitsPerWord(uint32_t payload_bytes, EccScheme scheme) {
    uint32_t wb = eccWordBytes(scheme);
    if (wb == 0) return payload_bytes * 8;
    return wb * 8;
}

inline bool isCorrelatedMode(EccGuard::FaultMode m) {
    // SingleWord and the spatial modes (Row/Column/Bank/Device) are modelled
    // as "all errors deposit into a single ECC word" -- that's exactly what
    // the chipkill family of codes is designed against. SingleCell is a
    // 1-bit fault so the distribution is degenerate either way.
    switch (m) {
    case EccGuard::FaultMode::SingleCell:    return false;
    case EccGuard::FaultMode::SingleWord:    return true;
    case EccGuard::FaultMode::SingleRow:     return true;
    case EccGuard::FaultMode::SingleColumn:  return true;
    case EccGuard::FaultMode::SingleBank:    return true;
    case EccGuard::FaultMode::SingleDevice:  return true;
    default:                                  return false;
    }
}

} // namespace

// Distribute `errs` bit-errors uniformly across chips in one word.
// For SingleDevice mode all errors land in a single randomly-chosen chip.
void EccGuard::distributeErrorsToChips(
        std::vector<uint8_t>& chip_counts,
        unsigned errs, EccScheme scheme, FaultMode mode) {
    unsigned nchips = chipsPerEccWord(scheme);
    if (nchips == 0 || errs == 0) return;
    chip_counts.assign(nchips, 0);
    if (campaign_force_multi_chip_ && nchips >= 3) {
        unsigned need = std::min<unsigned>(3u, nchips);
        std::vector<unsigned> picks;
        picks.reserve(need);
        std::uniform_int_distribution<unsigned> cpick(0, nchips - 1);
        while (picks.size() < need) {
            unsigned c = cpick(stdRng_);
            if (std::find(picks.begin(), picks.end(), c) == picks.end())
                picks.push_back(c);
        }
        unsigned per = std::max(1u, errs / need);
        unsigned rem = errs;
        for (size_t i = 0; i < picks.size(); ++i) {
            unsigned put = (i + 1 == picks.size()) ? rem : std::min(rem, per);
            chip_counts[picks[i]] = static_cast<uint8_t>(std::min<unsigned>(put, 255));
            rem -= put;
        }
        return;
    }
    if (mode == FaultMode::SingleDevice) {
        std::uniform_int_distribution<unsigned> cpick(0, nchips - 1);
        unsigned c = cpick(stdRng_);
        chip_counts[c] = static_cast<uint8_t>(std::min<unsigned>(errs, 4));
    } else {
        std::uniform_int_distribution<unsigned> cpick(0, nchips - 1);
        for (unsigned i = 0; i < errs; ++i) {
            unsigned c = cpick(stdRng_);
            if (chip_counts[c] < 255) ++chip_counts[c];
        }
    }
}

EccGuard::FaultDraw EccGuard::drawFaultPoisson(uint32_t payload_bytes,
                                               double ber,
                                               EccScheme scheme) {
    FaultDraw d;
    d.mode = FaultMode::SingleCell;
    if (payload_bytes == 0) return d;

    uint32_t nwords = numWords(payload_bytes, scheme);
    d.per_word_errors.assign(nwords, 0u);
    if (ber <= 0.0) return d;

    // Per-word independent Bernoulli/Poisson draws across the line. This is
    // the model that the publication-grade BER bound (see EccScheme.h,
    // kEccBerTightUpperBound) is provably tight for.
    double bpw = static_cast<double>(bitsPerWord(payload_bytes, scheme));
    std::poisson_distribution<unsigned> dist(bpw * ber);
    unsigned cap = bitsPerWord(payload_bytes, scheme);
    unsigned total = 0;
    bool need_chips = (scheme == EccScheme::CHIPKILL_x4);
    if (need_chips) d.per_word_chip_errors.resize(nwords);
    for (uint32_t w = 0; w < nwords; ++w) {
        unsigned errs = dist(stdRng_);
        if (cap > 0 && errs > cap) errs = cap;
        d.per_word_errors[w] = errs;
        total += errs;
        if (need_chips && errs > 0)
            distributeErrorsToChips(d.per_word_chip_errors[w], errs, scheme, d.mode);
    }
    d.num_errors = total;
    return d;
}

EccGuard::FaultDraw EccGuard::drawFaultJedecMix(uint32_t payload_bytes,
                                                double event_rate,
                                                EccScheme scheme) {
    FaultDraw d;
    if (payload_bytes == 0) return d;

    uint32_t nwords = numWords(payload_bytes, scheme);
    d.per_word_errors.assign(nwords, 0u);
    if (event_rate <= 0.0) return d;
    std::bernoulli_distribution gate(std::min(event_rate, 1.0));
    if (!gate(stdRng_)) return d;

    // Choose a mode by cumulative weight.
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    double r = u01(stdRng_);
    double acc = 0.0;
    int chosen = 0;
    for (int i = 0; i < kModeCount; ++i) {
        acc += mode_weights_[i];
        if (r <= acc) { chosen = i; break; }
    }
    d.mode = static_cast<FaultMode>(chosen);

    // Sample a bit-error count uniformly inside the mode's range; cap at payload bits.
    unsigned lo = kFaultModeBitsLow[chosen];
    unsigned hi = kFaultModeBitsHigh[chosen];
    if (hi < lo) hi = lo;
    std::uniform_int_distribution<unsigned> nbits(lo, hi);
    unsigned errs = nbits(stdRng_);
    unsigned cap  = payload_bytes * 8;
    if (cap > 0 && errs > cap) errs = cap;
    d.num_errors = errs;

    // Deposit the errors into word(s). Correlated/single-word modes drop the
    // whole event into one randomly-chosen word (faithfully modelling the
    // physical clustering that chipkill is meant to absorb); SingleCell
    // scatters across words uniformly bit-by-bit.
    bool need_chips = (scheme == EccScheme::CHIPKILL_x4);
    if (need_chips) d.per_word_chip_errors.resize(nwords);
    if (nwords > 0) {
        if (isCorrelatedMode(d.mode)) {
            std::uniform_int_distribution<uint32_t> wpick(0, nwords - 1);
            uint32_t w = wpick(stdRng_);
            unsigned word_cap = bitsPerWord(payload_bytes, scheme);
            if (word_cap > 0 && errs > word_cap) {
                d.per_word_errors[w] = word_cap;
                if (need_chips)
                    distributeErrorsToChips(d.per_word_chip_errors[w], word_cap, scheme, d.mode);
                unsigned remaining = errs - word_cap;
                uint32_t offset = 1;
                while (remaining > 0 && offset < nwords) {
                    uint32_t wn = (w + offset) % nwords;
                    unsigned put = std::min<unsigned>(word_cap, remaining);
                    d.per_word_errors[wn] = put;
                    if (need_chips)
                        distributeErrorsToChips(d.per_word_chip_errors[wn], put, scheme, d.mode);
                    remaining -= put;
                    ++offset;
                }
            } else {
                d.per_word_errors[w] = errs;
                if (need_chips)
                    distributeErrorsToChips(d.per_word_chip_errors[w], errs, scheme, d.mode);
            }
        } else {
            // Uncorrelated (SingleCell, default): scatter bit-by-bit.
            std::uniform_int_distribution<uint32_t> wpick(0, nwords - 1);
            for (unsigned i = 0; i < errs; ++i) {
                uint32_t w = wpick(stdRng_);
                d.per_word_errors[w] += 1;
            }
            if (need_chips) {
                for (uint32_t w = 0; w < nwords; ++w) {
                    if (d.per_word_errors[w] > 0)
                        distributeErrorsToChips(d.per_word_chip_errors[w],
                                                d.per_word_errors[w], scheme, d.mode);
                }
            }
        }
    }

    ++per_mode_draws_[chosen];
    if (chosen == static_cast<int>(FaultMode::SingleRow)    && stat_correlated_row_)    stat_correlated_row_->addData(1);
    if (chosen == static_cast<int>(FaultMode::SingleBank)   && stat_correlated_bank_)   stat_correlated_bank_->addData(1);
    if (chosen == static_cast<int>(FaultMode::SingleDevice) && stat_correlated_device_) stat_correlated_device_->addData(1);

    return d;
}

// Campaign-mode injector. Fires at most campaign_event_budget_ events total
// across the run, each event a single occurrence of campaign_mode_ deposited
// into one randomly-chosen ECC word (matching JEDEC's single-word/cell
// deposit semantics). Eligibility is gated on (kernel_name ==
// campaign_target_kernel_name_) when target is set, else any kernel is
// eligible.
EccGuard::FaultDraw EccGuard::drawFaultCampaign(uint32_t payload_bytes,
                                                 EccScheme scheme,
                                                 const std::string& kernel_name) {
    FaultDraw d;
    if (payload_bytes == 0) return d;

    uint32_t nwords = numWords(payload_bytes, scheme);
    d.per_word_errors.assign(nwords, 0u);

    if (campaign_event_budget_ == 0) return d;
    if (campaign_events_fired_ >= campaign_event_budget_) return d;
    // Addr-filtered campaign: action_queue traffic is the temporal proxy.
    // ReadResp often returns after publishKernel(IDLE), so do not gate on FSM.
    const bool addr_filtered = !addr_filter_region_.empty();
    if (!addr_filtered && !campaign_target_kernel_name_.empty()
        && kernel_name != campaign_target_kernel_name_) {
        return d;
    }
    if (addr_filtered && campaign_max_per_kernel_entry_ > 0 && state_ptr_) {
        const int pc = state_ptr_->pipelineCycle;
        if (pc != campaign_entry_pipeline_cycle_) {
            campaign_entry_pipeline_cycle_ = pc;
            campaign_events_this_entry_    = 0;
        }
    } else {
        noteCampaignKernelEntry(kernel_name);
    }
    if (campaign_max_per_kernel_entry_ > 0
        && campaign_events_this_entry_ >= campaign_max_per_kernel_entry_) {
        return d;
    }
    if (campaign_event_rate_ <= 0.0) return d;
    std::bernoulli_distribution gate(std::min(campaign_event_rate_, 1.0));
    if (!gate(stdRng_)) return d;

    d.mode = campaign_mode_;
    int chosen = static_cast<int>(campaign_mode_);

    unsigned lo = kFaultModeBitsLow [chosen];
    unsigned hi = kFaultModeBitsHigh[chosen];
    if (hi < lo) hi = lo;
    unsigned errs = 0;
    if (campaign_errors_fixed_ > 0) {
        errs = campaign_errors_fixed_;
    } else {
        std::uniform_int_distribution<unsigned> nbits(lo, hi);
        errs = nbits(stdRng_);
    }
    unsigned cap  = payload_bytes * 8;
    if (cap > 0 && errs > cap) errs = cap;
    d.num_errors = errs;

    bool need_chips = (scheme == EccScheme::CHIPKILL_x4);
    if (need_chips) d.per_word_chip_errors.resize(nwords);
    if (nwords > 0) {
        if (isCorrelatedMode(d.mode)) {
            std::uniform_int_distribution<uint32_t> wpick(0, nwords - 1);
            uint32_t w = wpick(stdRng_);
            unsigned word_cap = bitsPerWord(payload_bytes, scheme);
            if (word_cap > 0 && errs > word_cap) {
                d.per_word_errors[w] = word_cap;
                if (need_chips)
                    distributeErrorsToChips(d.per_word_chip_errors[w], word_cap, scheme, d.mode);
                unsigned remaining = errs - word_cap;
                uint32_t offset = 1;
                while (remaining > 0 && offset < nwords) {
                    uint32_t wn = (w + offset) % nwords;
                    unsigned put = std::min<unsigned>(word_cap, remaining);
                    d.per_word_errors[wn] = put;
                    if (need_chips)
                        distributeErrorsToChips(d.per_word_chip_errors[wn], put, scheme, d.mode);
                    remaining -= put;
                    ++offset;
                }
            } else {
                d.per_word_errors[w] = errs;
                if (need_chips)
                    distributeErrorsToChips(d.per_word_chip_errors[w], errs, scheme, d.mode);
            }
        } else {
            std::uniform_int_distribution<uint32_t> wpick(0, nwords - 1);
            for (unsigned i = 0; i < errs; ++i) {
                d.per_word_errors[wpick(stdRng_)] += 1;
            }
            if (need_chips) {
                for (uint32_t w = 0; w < nwords; ++w) {
                    if (d.per_word_errors[w] > 0)
                        distributeErrorsToChips(d.per_word_chip_errors[w],
                                                d.per_word_errors[w], scheme, d.mode);
                }
            }
        }
    }

    ++campaign_events_fired_;
    ++campaign_events_this_entry_;
    ++per_mode_draws_[chosen];
    if (chosen == static_cast<int>(FaultMode::SingleRow)    && stat_correlated_row_)    stat_correlated_row_->addData(1);
    if (chosen == static_cast<int>(FaultMode::SingleBank)   && stat_correlated_bank_)   stat_correlated_bank_->addData(1);
    if (chosen == static_cast<int>(FaultMode::SingleDevice) && stat_correlated_device_) stat_correlated_device_->addData(1);
    return d;
}

void EccGuard::warnIfBerExceedsTightBound(double ber, const char* origin) {
    if (ber <= kEccBerTightUpperBound) return;
    // Memoize so repeated BER values don't spam the log.
    uint64_t key = 0;
    std::memcpy(&key, &ber, sizeof(key));
    if (!ber_warned_.insert(key).second) return;
    if (out_) {
        out_->output(
            "EccGuard '%s': WARNING %s BER=%.3e exceeds the per-word "
            "single-bit approximation's tight bound (%.1e). The per-word "
            "draws still classify each event correctly, but the "
            "Correctable/DUE/Escape proportions are no longer provably "
            "tight to within ~1%% of an exact Binomial decode. See "
            "EccScheme.h::kEccBerTightUpperBound for the derivation.\n",
            getName().c_str(), origin, ber, kEccBerTightUpperBound);
    }
}

uint64_t EccGuard::applyPolicy(MemEvent* mev) {
    if (!state_ptr_) resolveStateLazy();

    std::string kernel_name;
    if (state_ptr_) kernel_name = state_ptr_->currentKernelName;

    if (!addr_filter_region_.empty() && !eventOverlapsAddrFilter(mev)) {
        if (stat_total_) stat_total_->addData(1);
        if (stat_clean_) stat_clean_->addData(1);
        return 0;
    }

    int region_id = resolveRegionIdForEvent(mev);
    const std::string& region_name = regionNameForId(region_id);

    const EccPolicyEntry& entry = policy_.effectiveFor(kernel_name, region_name);

    auto& kernel_bucket = per_kernel_[kernel_name];
    auto& region_bucket = per_kernel_region_[std::make_pair(kernel_name, region_name)];

    auto countClean = [&]() {
        if (stat_total_) stat_total_->addData(1);
        if (stat_clean_) stat_clean_->addData(1);
        kernel_bucket.clean += 1;
        region_bucket.clean += 1;
    };

    if (entry.ber <= 0.0 && fault_event_rate_ <= 0.0
        && entry.scheme == EccScheme::NONE
        && fault_model_ != FaultModel::Campaign) {
        countClean();
        return 0;
    }

    auto& payload = mev->getPayload();
    if (payload.empty()) {
        countClean();
        return 0;
    }

    uint32_t payload_bytes = static_cast<uint32_t>(payload.size());

    FaultDraw draw;
    if (fault_model_ == FaultModel::JedecMix) {
        // event_rate priority: per-policy BER (if user wants per-kernel/region rates)
        // falls back to the guard-level fault_event_rate.
        double rate = (entry.ber > 0.0) ? entry.ber : fault_event_rate_;
        draw = drawFaultJedecMix(payload_bytes, rate, entry.scheme);
    } else if (fault_model_ == FaultModel::Campaign) {
        draw = drawFaultCampaign(payload_bytes, entry.scheme, kernel_name);
    } else {
        draw = drawFaultPoisson(payload_bytes, entry.ber, entry.scheme);
    }

    EccLineOutcome line = draw.per_word_chip_errors.empty()
        ? aggregateLineOutcome(draw.per_word_errors, entry.scheme)
        : aggregateLineOutcomeChipAware(draw.per_word_errors,
                                        draw.per_word_chip_errors, entry.scheme);
    EccOutcome     outcome = line.outcome;

    uint64_t latency_ps = 0;
    bool high_blast_flip = false;
    switch (outcome) {
    case EccOutcome::Clean:
        latency_ps = 0;
        break;
    case EccOutcome::Correctable:
        latency_ps = entry.correctable_latency_ps;
        break;
    case EccOutcome::DetectableUncorrectable:
        latency_ps = entry.due_latency_ps;
        if (due_action_ == DueAction::DropFrame) {
            requestFrameAbort();
        }
        break;
    case EccOutcome::SilentEscape:
        latency_ps = entry.escape_latency_ps;
        // Only bits in words whose own ECC decode escaped actually corrupt
        // the line on the wire (line.escape_bits). Errors in Correctable
        // words are fixed; errors in DUE words are masked by the DUE event
        // itself in either drop_frame or latency_only paths. This is the
        // per-word model's contribution vs the old single-word approximation
        // which flipped every error on the line.
        for (unsigned i = 0; i < line.escape_bits && !payload.empty(); ++i) {
            bool wasHi = false;
            if (payload_dtype_ == PayloadDtype::Bytes) {
                flipRandomBit(mev, wasHi);
            } else {
                flipDataTypeAware(mev, wasHi);
            }
            if (wasHi) {
                ++escape_high_blast_total_;
                if (stat_escape_high_blast_) stat_escape_high_blast_->addData(1);
                high_blast_flip = true;
            } else {
                ++escape_low_blast_total_;
                if (stat_escape_low_blast_) stat_escape_low_blast_->addData(1);
            }
        }
        publishCumulative(state_key_, /*escapes*/1, /*flips*/line.escape_bits);
        // Tier B (Fig. 3a) violation attribution: this escape happened
        // while currentKernelName was kernel_name; bump the per-frame map so
        // the pipeline agent can argmax it at frame close.
        publishPerFrameEscape(state_key_, kernel_name);
        break;
    }

    if (stat_total_) stat_total_->addData(1);
    switch (outcome) {
    case EccOutcome::Clean:
        if (stat_clean_)       stat_clean_->addData(1);
        kernel_bucket.clean += 1;
        region_bucket.clean += 1;
        break;
    case EccOutcome::Correctable:
        if (stat_correctable_) stat_correctable_->addData(1);
        kernel_bucket.correctable += 1;
        region_bucket.correctable += 1;
        break;
    case EccOutcome::DetectableUncorrectable:
        if (stat_due_)         stat_due_->addData(1);
        kernel_bucket.due += 1;
        region_bucket.due += 1;
        break;
    case EccOutcome::SilentEscape:
        if (stat_escape_)      stat_escape_->addData(1);
        kernel_bucket.escape += 1;
        region_bucket.escape += 1;
        break;
    }
    if (latency_ps > 0) {
        if (stat_latency_) stat_latency_->addData(latency_ps);
        kernel_bucket.latency_ps += latency_ps;
        region_bucket.latency_ps += latency_ps;
    }
    if (verbose_ && (outcome != EccOutcome::Clean || high_blast_flip)) {
        out_->output("EccGuard '%s': kernel=%s region=%s mode=%s "
                     "errors=%u (escape_bits=%u over %zu words) outcome=%s "
                     "+%" PRIu64 " ps\n",
                     getName().c_str(),
                     kernel_name.empty() ? "UNKNOWN" : kernel_name.c_str(),
                     region_name.empty() ? "unlabeled" : region_name.c_str(),
                     faultModeName(draw.mode),
                     draw.num_errors, line.escape_bits,
                     draw.per_word_errors.size(),
                     eccOutcomeName(outcome), latency_ps);
    }

    return latency_ps;
}

bool EccGuard::flipDataTypeAware(MemEvent* mev, bool& wasHighBlast) {
    auto& payload = mev->getPayload();
    if (payload.empty()) { wasHighBlast = false; return false; }
    unsigned elem_bytes = dtypeBytes(payload_dtype_);
    if (elem_bytes == 0) elem_bytes = 1;

    uint32_t total_bytes = static_cast<uint32_t>(payload.size());
    uint32_t num_elems   = total_bytes / elem_bytes;
    if (num_elems == 0) {
        flipRandomBit(mev, wasHighBlast);
        return true;
    }

    uint32_t elem_idx     = rng_.generateNextUInt32() % num_elems;
    uint32_t bit_in_elem  = rng_.generateNextUInt32() % (elem_bytes * 8u);
    uint32_t byte_in_elem = bit_in_elem / 8u;
    uint32_t bit_in_byte  = bit_in_elem % 8u;
    // bf16 stored little-endian: byte 0 = mantissa low, byte 1 = sign+exp high.
    // Element-relative bit numbering treats bit 0 as element LSB, so for bf16:
    //   bit_in_elem [0..7]  -> byte 0
    //   bit_in_elem [8..15] -> byte 1
    uint32_t global_byte = elem_idx * elem_bytes + byte_in_elem;
    if (global_byte >= total_bytes) {
        wasHighBlast = false;
        return false;
    }
    payload[global_byte] ^= static_cast<uint8_t>(1u << bit_in_byte);
    wasHighBlast = isHighBlastBit(payload_dtype_, bit_in_elem);
    return true;
}

void EccGuard::flipRandomBit(MemEvent* mev, bool& wasHighBlast) {
    auto& payload = mev->getPayload();
    if (payload.empty()) { wasHighBlast = false; return; }
    uint32_t byte = rng_.generateNextUInt32() % static_cast<uint32_t>(payload.size());
    uint32_t bit  = rng_.generateNextUInt32() % 8u;
    payload[byte] ^= static_cast<uint8_t>(1u << bit);
    wasHighBlast = false;
}
