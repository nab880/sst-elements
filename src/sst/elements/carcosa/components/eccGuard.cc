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

#include "sst_config.h"
#include "sst/elements/carcosa/components/eccGuard.h"
#include "sst/elements/carcosa/components/eccModelMath.h"
#include "sst/elements/carcosa/components/memEventPayload.h"
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

bool parseFaultModel(const std::string& s, EccGuard::FaultModel& model) {
    if (s != "poisson" && s != "POISSON") return false;
    model = EccGuard::FaultModel::Poisson;
    return true;
}

bool parseDueAction(const std::string& s, EccGuard::DueAction& action) {
    if (s != "latency_only" && s != "LATENCY_ONLY") return false;
    action = EccGuard::DueAction::LatencyOnly;
    return true;
}

} // namespace

EccGuard::EccGuard(ComponentId_t id, Params& params) : Component(id) {
    requireLibrary("memHierarchy");

    out_ = new Output("", 1, 0, Output::STDOUT);
    verbose_ = params.find<bool>("verbose", false);
    test_bounds_.add("total", params.find<int64_t>("test_total_min", -1),
                     params.find<int64_t>("test_total_max", -1));
    test_bounds_.add("clean", params.find<int64_t>("test_clean_min", -1),
                     params.find<int64_t>("test_clean_max", -1));
    test_bounds_.add("correctable", params.find<int64_t>("test_correctable_min", -1),
                     params.find<int64_t>("test_correctable_max", -1));
    test_bounds_.add("due", params.find<int64_t>("test_due_min", -1),
                     params.find<int64_t>("test_due_max", -1));
    test_bounds_.add("escape", params.find<int64_t>("test_escape_min", -1),
                     params.find<int64_t>("test_escape_max", -1));

    state_key_            = params.find<std::string>("state_key", "");
    apply_on_responses_only_ = params.find<bool>("apply_on_responses_only", true);

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
        if (!errors.empty()) {
            out_->fatal(CALL_INFO, -1, "EccGuard: invalid kernel_policy.\n");
        }
        if (verbose_) {
            out_->output("EccGuard: parsed %d kernel/region policy override(s).\n", parsed);
        }
    }

    std::string fault_model = params.find<std::string>("fault_model", "poisson");
    std::string payload_dtype = params.find<std::string>("payload_dtype", "bytes");
    std::string due_action = params.find<std::string>("due_action", "latency_only");
    if (!parseFaultModel(fault_model, fault_model_)) {
        out_->fatal(CALL_INFO, -1, "EccGuard: unsupported fault_model '%s'; this component supports only poisson.\n",
                    fault_model.c_str());
    }
    if (!EccPayloadCorruptor::parseDtype(payload_dtype, payload_dtype_)) {
        out_->fatal(CALL_INFO, -1, "EccGuard: unknown payload_dtype '%s'.\n",
                    payload_dtype.c_str());
    }
    if (!parseDueAction(due_action, due_action_)) {
        out_->fatal(CALL_INFO, -1, "EccGuard: unsupported due_action '%s'; only latency_only is available without frame integration.\n",
                    due_action.c_str());
    }

    addr_filter_region_ = params.find<std::string>("addr_filter_region", "");
    addr_filter_len_ = params.find<uint64_t>("addr_filter_len", 0);
    inject_addr_start_ = params.find<uint64_t>("inject_addr_start", 0);
    inject_addr_len_ = params.find<uint64_t>("inject_addr_len", 0);

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
    stat_escape_high_blast_  = registerStatistic<uint64_t>("escape_high_blast");
    stat_escape_low_blast_   = registerStatistic<uint64_t>("escape_low_blast");
    stat_due_poisoned_       = registerStatistic<uint64_t>("due_poisoned_bits");
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

    // Warn in setup() for every policy BER above kEccBerTightUpperBound so
    // reviewers see the bound before the sim produces numbers.
    policy_.forEachEntry([&](const std::string& origin, const EccPolicyEntry& e) {
        if (e.ber > 0.0) {
            warnIfBerExceedsTightBound(e.ber, origin.c_str());
        }
    });


    if (verbose_) {
        out_->output("EccGuard '%s': setup. uniform scheme=%s ber=%g state_key='%s' state_ptr=%p "
                     "fault_model=%s payload_dtype=%s due_action=%s\n",
                     getName().c_str(),
                     eccSchemeName(policy_.uniform().scheme),
                     policy_.uniform().ber,
                     state_key_.c_str(),
                     (const void*)state_ptr_,
                     "poisson",
                     EccPayloadCorruptor::dtypeName(payload_dtype_),
                     "latency_only");
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
    OutcomeCounters totals;
    for (const auto& kv : per_kernel_) {
        totals.clean += kv.second.clean;
        totals.correctable += kv.second.correctable;
        totals.due += kv.second.due;
        totals.escape += kv.second.escape;
    }
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

    if (escape_high_blast_total_ + escape_low_blast_total_ > 0
        || due_poison_flips_total_ > 0) {
        out_->output("\n=== EccGuard %s Escape Summary ===\n", getName().c_str());
        out_->output("escape_high_blast,escape_low_blast,payload_dtype,due_poisoned_bits\n");
        out_->output("%" PRIu64 ",%" PRIu64 ",%s,%" PRIu64 "\n",
                     escape_high_blast_total_, escape_low_blast_total_,
                     EccPayloadCorruptor::dtypeName(payload_dtype_),
                     due_poison_flips_total_);
        out_->output("=== End EccGuard %s Escape Summary ===\n\n", getName().c_str());
    }


    const uint64_t total = totals.clean + totals.correctable + totals.due + totals.escape;
    test_bounds_.set("total", total);
    test_bounds_.set("clean", totals.clean);
    test_bounds_.set("correctable", totals.correctable);
    test_bounds_.set("due", totals.due);
    test_bounds_.set("escape", totals.escape);
    if (!test_bounds_.check(*out_, getName())) {
        out_->fatal(CALL_INFO, -1, "EccGuard test expectations failed.\n");
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

// Region attribution: EccGuard sees physical addrs below the dTLB, but agents
// publish virtual regions. Prefer MemEvent::vAddr_ (stamped by dTLB, preserved
// by clone/makeResponse); fall back to physical when vAddr=0 (e.g. writebacks).
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
    return !apply_on_responses_only_ || mev->isResponse();
}

bool EccGuard::eventOverlapsAddrFilter(MemEvent* mev) const {
    if (addr_filter_region_.empty() || !mev) return true;
    uint64_t fbase = 0, flen = 0;
    if (!resolveAddrFilterBounds(fbase, flen)) return false;
    const uint64_t addr = memEventPayloadAddress(*mev);
    const uint64_t size = mev->getPayloadSize() != 0
        ? mev->getPayloadSize() : mev->getSize();
    return size != 0 && (addr < fbase ? fbase - addr < size : addr - fbase < flen);
}



namespace {
// Publish generic cumulative ECC counters for consumers sharing this state key.
void publishCumulative(const std::string& state_key, uint64_t escapes_inc,
                       uint64_t flips_inc) {
    if (state_key.empty()) return;
    PipelineStateBase* s =
        PipelineStateRegistry<PipelineStateBase>::getMutable(state_key);
    if (!s) return;
    s->addEccCounts(escapes_inc, flips_inc);
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
    return EccModelMath::wordCount(payload_bytes, scheme);
}

} // namespace

// Scatter Poisson bit errors across x4 chips for chip-aware classification.
void EccGuard::distributeErrorsToChips(
        std::vector<uint8_t>& chip_counts, unsigned errs, EccScheme scheme) {
    unsigned nchips = chipsPerEccWord(scheme);
    if (nchips == 0 || errs == 0) return;
    chip_counts.assign(nchips, 0);
    std::uniform_int_distribution<unsigned> cpick(0, nchips - 1);
    for (unsigned i = 0; i < errs; ++i) {
        unsigned c = cpick(stdRng_);
        if (chip_counts[c] < 255) ++chip_counts[c];
    }
}

EccGuard::FaultDraw EccGuard::drawFaultPoisson(uint32_t payload_bytes,
                                               double ber,
                                               EccScheme scheme) {
    FaultDraw d;
    if (payload_bytes == 0) return d;

    uint32_t nwords = numWords(payload_bytes, scheme);
    d.per_word_errors.assign(nwords, 0u);
    if (ber <= 0.0) return d;

    // Per-word Bernoulli/Poisson draws; partial final words use actual bit count.
    unsigned total = 0;
    bool need_chips = (scheme == EccScheme::CHIPKILL_x4);
    if (need_chips) d.per_word_chip_errors.resize(nwords);
    for (uint32_t w = 0; w < nwords; ++w) {
        unsigned word_bits = EccModelMath::wordBits(payload_bytes, scheme, w);
        if (word_bits == 0) break;
        std::poisson_distribution<unsigned> dist(static_cast<double>(word_bits) * ber);
        unsigned errs = dist(stdRng_);
        if (errs > word_bits) errs = word_bits;
        d.per_word_errors[w] = errs;
        total += errs;
        if (need_chips && errs > 0)
            distributeErrorsToChips(d.per_word_chip_errors[w], errs, scheme);
    }
    d.num_errors = total;
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
            "eccScheme.h::kEccBerTightUpperBound for the derivation.\n",
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

    // Raw inject window (no region registry): confine to [start, start+len).
    // Prefer preserved vAddr; fall back to physical (e.g. balar H2D path).
    if (inject_addr_len_ > 0) {
        const uint64_t a = memEventPayloadAddress(*mev);
        const uint64_t sz = mev->getPayloadSize() != 0
            ? mev->getPayloadSize() : mev->getSize();
        const bool overlaps = sz != 0 && (a < inject_addr_start_
            ? inject_addr_start_ - a < sz : a - inject_addr_start_ < inject_addr_len_);
        if (!overlaps) {
            if (stat_total_) stat_total_->addData(1);
            if (stat_clean_) stat_clean_->addData(1);
            return 0;
        }
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

    if (entry.ber <= 0.0 && entry.scheme == EccScheme::NONE) {
        countClean();
        return 0;
    }

    if (mev->getPayloadSize() == 0) {
        countClean();
        return 0;
    }

    uint32_t payload_bytes = static_cast<uint32_t>(mev->getPayloadSize());

    FaultDraw draw = drawFaultPoisson(payload_bytes, entry.ber, entry.scheme);

    EccLineOutcome line = draw.per_word_chip_errors.empty()
        ? aggregateLineOutcome(draw.per_word_errors, entry.scheme)
        : aggregateLineOutcomeChipAware(draw.per_word_errors,
                                        draw.per_word_chip_errors, entry.scheme);
    EccOutcome     outcome = line.outcome;

    // DUE words forward poison by flipping their drawn error bits into the
    // payload, including when another word on the line silently escapes.
    auto handleDueWords = [&]() {
        if (line.due_words.empty()) return;
        unsigned flips = 0;
        for (uint32_t w : line.due_words) {
            EccPayloadFlipCount count = EccPayloadCorruptor::flipRandom(
                *mev, w, entry.scheme, draw.per_word_errors[w], payload_dtype_, rng_);
            flips += count.total;
        }
        due_poison_flips_total_ += flips;
        if (stat_due_poisoned_) stat_due_poisoned_->addData(flips);
        publishCumulative(state_key_, /*escapes*/0, /*flips*/flips);
    };

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
        handleDueWords();
        break;
    case EccOutcome::SilentEscape: {
        latency_ps = entry.escape_latency_ps;
        // Corrupt only words whose ECC decode escaped (per_word_errors[w] bits).
        // Correctable words leak nothing; DUE words get the DUE response below.
        unsigned hi = 0, lo = 0, flips = 0;
        for (uint32_t w : line.escape_words) {
            EccPayloadFlipCount count = EccPayloadCorruptor::flipRandom(
                *mev, w, entry.scheme, draw.per_word_errors[w], payload_dtype_, rng_);
            flips += count.total;
            hi += count.high;
            lo += count.low;
        }
        escape_high_blast_total_ += hi;
        escape_low_blast_total_  += lo;
        if (hi && stat_escape_high_blast_) stat_escape_high_blast_->addData(hi);
        if (lo && stat_escape_low_blast_)  stat_escape_low_blast_->addData(lo);
        high_blast_flip = (hi > 0);
        publishCumulative(state_key_, /*escapes*/1, /*flips*/flips);
        handleDueWords();
        if (!line.due_words.empty() && entry.due_latency_ps > latency_ps)
            latency_ps = entry.due_latency_ps;
        break;
    }
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
        out_->output("EccGuard '%s': addr=0x%llx vaddr=0x%llx kernel=%s region=%s "
                     "errors=%u (escape_bits=%u over %zu words) outcome=%s "
                     "+%" PRIu64 " ps\n",
                     getName().c_str(),
                     (unsigned long long)mev->getAddr(),
                     (unsigned long long)mev->getVirtualAddress(),
                     kernel_name.empty() ? "UNKNOWN" : kernel_name.c_str(),
                     region_name.empty() ? "unlabeled" : region_name.c_str(),
                     draw.num_errors, line.escape_bits,
                     draw.per_word_errors.size(),
                     eccOutcomeName(outcome), latency_ps);
    }

    return latency_ps;
}
