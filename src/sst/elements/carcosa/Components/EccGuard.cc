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
#include <cinttypes>
#include <cstring>

using namespace SST;
using namespace SST::MemHierarchy;
using namespace SST::Carcosa;

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
            out_->output("EccGuard: parsed %d kernel-policy override(s).\n", parsed);
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

    stat_total_       = registerStatistic<uint64_t>("events_total");
    stat_clean_       = registerStatistic<uint64_t>("events_clean");
    stat_correctable_ = registerStatistic<uint64_t>("events_correctable");
    stat_due_         = registerStatistic<uint64_t>("events_due");
    stat_escape_      = registerStatistic<uint64_t>("events_escape");
    stat_latency_     = registerStatistic<uint64_t>("latency_added_ps");
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
    if (verbose_) {
        out_->output("EccGuard '%s': setup. uniform scheme=%s ber=%g state_key='%s' state_ptr=%p\n",
                     getName().c_str(),
                     eccSchemeName(policy_.uniform().scheme),
                     policy_.uniform().ber,
                     state_key_.c_str(),
                     (const void*)state_ptr_);
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
    out_->output("kernel_id,kernel_name,clean,correctable,due,escape,latency_ps\n");
    for (int i = 0; i < NUM_STATES; ++i) {
        const auto& c = per_kernel_[i];
        if (c.clean + c.correctable + c.due + c.escape == 0) continue;
        out_->output("%d,%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                     i, vlaStateName(i),
                     c.clean, c.correctable, c.due, c.escape, c.latency_ps);
    }
    const auto& unk = per_kernel_[NUM_STATES];
    if (unk.clean + unk.correctable + unk.due + unk.escape != 0) {
        out_->output("-1,UNKNOWN,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                     unk.clean, unk.correctable, unk.due, unk.escape, unk.latency_ps);
    }
    out_->output("=== End EccGuard %s Per-Kernel Outcomes ===\n\n", getName().c_str());
}

void EccGuard::resolveStateLazy() {
    if (state_ptr_ || state_key_.empty()) return;
    state_ptr_ = PipelineStateRegistry<PipelineStateBase>::get(state_key_);
}

void EccGuard::handleHighlink(SST::Event* ev) {
    auto* mev = dynamic_cast<MemEvent*>(ev);
    if (!mev || (applyOnResponsesOnly_ && !mev->isResponse())) {
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
    if (applyOnResponsesOnly_ && !mev->isResponse()) {
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

uint64_t EccGuard::applyPolicy(MemEvent* mev) {
    // Lazy resolve: first events may arrive before the agent has registered.
    if (!state_ptr_) resolveStateLazy();

    int kernel_id = -1;
    if (state_ptr_) kernel_id = state_ptr_->currentKernel;

    const EccPolicyEntry& entry = policy_.effectiveFor(kernel_id);

    if (entry.ber <= 0.0 && entry.scheme == EccScheme::NONE) {
        if (stat_total_) stat_total_->addData(1);
        if (stat_clean_) stat_clean_->addData(1);
        int idx = (kernel_id >= 0 && kernel_id < NUM_STATES) ? kernel_id : NUM_STATES;
        per_kernel_[idx].clean += 1;
        return 0;
    }

    auto& payload = mev->getPayload();
    if (payload.empty()) {
        if (stat_total_) stat_total_->addData(1);
        if (stat_clean_) stat_clean_->addData(1);
        int idx = (kernel_id >= 0 && kernel_id < NUM_STATES) ? kernel_id : NUM_STATES;
        per_kernel_[idx].clean += 1;
        return 0;
    }

    uint32_t payload_bytes = static_cast<uint32_t>(payload.size());
    double   payload_bits  = static_cast<double>(payload_bytes) * 8.0;

    // Poisson approximates Binomial(N, p) when p << 1 (our regime by design).
    unsigned num_errors = 0;
    if (entry.ber > 0.0) {
        std::poisson_distribution<unsigned> dist(payload_bits * entry.ber);
        num_errors = dist(stdRng_);
    }

    EccOutcome outcome = classifyEccOutcome(num_errors, payload_bytes, entry.scheme);

    uint64_t latency_ps = 0;
    switch (outcome) {
    case EccOutcome::Clean:                   latency_ps = 0;                          break;
    case EccOutcome::Correctable:             latency_ps = entry.correctable_latency_ps; break;
    case EccOutcome::DetectableUncorrectable: latency_ps = entry.due_latency_ps;       break;
    case EccOutcome::SilentEscape:
        latency_ps = entry.escape_latency_ps;
        for (unsigned i = 0; i < num_errors && !payload.empty(); ++i) {
            flipRandomBit(mev);
        }
        break;
    }

    if (stat_total_) stat_total_->addData(1);
    int idx = (kernel_id >= 0 && kernel_id < NUM_STATES) ? kernel_id : NUM_STATES;
    switch (outcome) {
    case EccOutcome::Clean:
        if (stat_clean_)       stat_clean_->addData(1);
        per_kernel_[idx].clean += 1;
        break;
    case EccOutcome::Correctable:
        if (stat_correctable_) stat_correctable_->addData(1);
        per_kernel_[idx].correctable += 1;
        break;
    case EccOutcome::DetectableUncorrectable:
        if (stat_due_)         stat_due_->addData(1);
        per_kernel_[idx].due += 1;
        break;
    case EccOutcome::SilentEscape:
        if (stat_escape_)      stat_escape_->addData(1);
        per_kernel_[idx].escape += 1;
        break;
    }
    if (latency_ps > 0) {
        if (stat_latency_) stat_latency_->addData(latency_ps);
        per_kernel_[idx].latency_ps += latency_ps;
        if (verbose_) {
            out_->output("EccGuard '%s': kernel=%d (%s) errors=%u outcome=%s +%" PRIu64 " ps\n",
                         getName().c_str(), kernel_id,
                         (kernel_id >= 0 && kernel_id < NUM_STATES) ? vlaStateName(kernel_id) : "UNKNOWN",
                         num_errors, eccOutcomeName(outcome), latency_ps);
        }
    }

    return latency_ps;
}

void EccGuard::flipRandomBit(MemEvent* mev) {
    auto& payload = mev->getPayload();
    if (payload.empty()) return;
    uint32_t byte = rng_.generateNextUInt32() % static_cast<uint32_t>(payload.size());
    uint32_t bit  = rng_.generateNextUInt32() % 8u;
    payload[byte] ^= static_cast<uint8_t>(1u << bit);
}
