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

#include "sst/elements/carcosa/injectors/portModuleStateGate.h"
#include "sst/elements/carcosa/faultlogic/randomFlipFault.h"
#include "sst/core/params.h"

#include "sst/elements/memHierarchy/memEvent.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace SST::Carcosa;

namespace {

std::string trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::vector<std::string> splitCsv(const std::string& csv) {
    std::vector<std::string> out;
    std::stringstream ss(csv);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        tok = trim(tok);
        if (!tok.empty()) out.push_back(tok);
    }
    return out;
}

std::set<int> parseIntSet(const std::string& csv) {
    std::set<int> out;
    for (const auto& tok : splitCsv(csv)) {
        try { out.insert(std::stoi(tok)); } catch (...) { /* skip bad tokens */ }
    }
    return out;
}

std::set<std::string> parseStringSet(const std::string& csv) {
    std::set<std::string> out;
    for (auto& tok : splitCsv(csv)) out.insert(std::move(tok));
    return out;
}

} // namespace

PortModuleStateGate::Mode
PortModuleStateGate::parseMode(const std::string& s) {
    std::string v;
    v.reserve(s.size());
    for (char c : s) v.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (v == "drop")      return Mode::Drop;
    if (v == "flip")      return Mode::Flip;
    if (v == "drop_flip" || v == "dropflip" || v == "both") return Mode::DropFlip;
    return Mode::Drop;
}

PortModuleStateGate::PortModuleStateGate(Params& params)
    : FaultInjectorBase(params)
{
    stateKey_ = params.find<std::string>("state_key", "");
    if (stateKey_.empty()) {
        out_->fatal(CALL_INFO_LONG, -1,
                    "PortModuleStateGate: 'state_key' is required.\n");
    }

    mode_     = parseMode(params.find<std::string>("fault_mode", "drop"));
    dropProb_ = params.find<double>("drop_probability", 1.0);
    flipProb_ = params.find<double>("flip_probability", 1.0);

    // Fixed two-slot layout so subclasses can rely on [drop=0, flip=1].
    // We handle drop inline (see executeFaults) because it's just a
    // cancelDelivery() on any Event subtype, not a MemEvent-payload op
    // like RandomDropFault/RandomFlipFault assume. fault[0] stays null.
    // fault[1] wraps RandomFlipFault only when flip mode is enabled;
    // RandomFlipFault does require a MemEvent payload to operate on.
    fault.resize(2);
    fault[0] = nullptr;
    fault[1] = (mode_ == Mode::Flip || mode_ == Mode::DropFlip)
                 ? new RandomFlipFault(params, this)
                 : nullptr;

    buildPredicates(params);

    // Per-event address filter. Turns on whenever the caller supplied region_ids
    // or region_names (keeping the gate focused on the published regions
    // they already care about). Can be force-disabled via match_event_address=0
    // for callers that want the pre-existing state-only semantics.
    const std::string regionIdsStr   = params.find<std::string>("region_ids", "");
    const std::string regionNamesStr = params.find<std::string>("region_names", "");
    const bool hasAnyRegionFilter    = !regionIdsStr.empty() || !regionNamesStr.empty();
    const bool wantAddressMatch      = params.find<bool>("match_event_address", hasAnyRegionFilter);
    matchEventAddress_ = hasAnyRegionFilter && wantAddressMatch;
    if (matchEventAddress_) {
        addrFilterIds_   = parseIntSet(regionIdsStr);
        addrFilterNames_ = parseStringSet(regionNamesStr);
    }

    logInjections_ = params.find<bool>("log_injections", true);

    setValidInstallation(params, SEND_RECEIVE_VALID);

#ifdef __SST_DEBUG_OUTPUT__
    dbg_->debug(CALL_INFO_LONG, 1, 0,
                "PortModuleStateGate: state_key='%s' mode=%d drop_p=%f flip_p=%f "
                "predicates=%zu\n",
                stateKey_.c_str(), static_cast<int>(mode_),
                dropProb_, flipProb_, predicates_.size());
#endif
}

void
PortModuleStateGate::buildPredicates(Params& params)
{
    // kernel-id set predicate: matches when currentKernel is in the set.
    const std::string kernelsStr = params.find<std::string>("kernels", "");
    if (!kernelsStr.empty()) {
        auto allowed = parseIntSet(kernelsStr);
        predicates_.emplace_back(
            [allowed = std::move(allowed)](const PipelineStateBase& s) {
                return allowed.count(s.currentKernel) > 0;
            });
    }

    // pipeline cycle range predicate: [start, end] inclusive; either bound optional.
    // Use a large sentinel for "unset" to keep the comparison branch-free.
    const bool hasStart = params.contains("pipeline_cycle_start");
    const bool hasEnd   = params.contains("pipeline_cycle_end");
    if (hasStart || hasEnd) {
        const int start = params.find<int>("pipeline_cycle_start", 0);
        const int end   = params.find<int>("pipeline_cycle_end", INT32_MAX);
        predicates_.emplace_back(
            [start, end](const PipelineStateBase& s) {
                return s.pipelineCycle >= start && s.pipelineCycle <= end;
            });
    }

    // region-id set predicate: matches if any valid region in s.regions has a matching id.
    const std::string regionIdsStr = params.find<std::string>("region_ids", "");
    if (!regionIdsStr.empty()) {
        auto allowed = parseIntSet(regionIdsStr);
        predicates_.emplace_back(
            [allowed = std::move(allowed)](const PipelineStateBase& s) {
                for (const auto& r : s.regions) {
                    if (r.valid && allowed.count(r.id) > 0) return true;
                }
                return false;
            });
    }

    // region-name set predicate: matches if any valid region has a matching name.
    const std::string regionNamesStr = params.find<std::string>("region_names", "");
    if (!regionNamesStr.empty()) {
        auto allowed = parseStringSet(regionNamesStr);
        predicates_.emplace_back(
            [allowed = std::move(allowed)](const PipelineStateBase& s) {
                for (const auto& r : s.regions) {
                    if (r.valid && allowed.count(r.name) > 0) return true;
                }
                return false;
            });
    }
}

bool
PortModuleStateGate::matchesState(const PipelineStateBase& state) const
{
    for (const auto& p : predicates_) {
        if (!p(state)) return false;
    }
    return true;
}

bool
PortModuleStateGate::eventMatchesAddressFilter(Event* ev) const
{
    if (!matchEventAddress_) return true;

    auto* mem_ev = dynamic_cast<SST::MemHierarchy::MemEvent*>(ev);
    if (!mem_ev) {
        // Non-MemEvents can't be meaningfully address-filtered; let them
        // through so drop-mode gates on generic Event subtypes still work.
        return true;
    }

    const PipelineStateBase* state =
        PipelineStateRegistry<PipelineStateBase>::get(stateKey_);
    if (!state) return false;

    const uint64_t addr = mem_ev->getBaseAddr();
    for (const auto& r : state->regions) {
        if (!r.valid) continue;
        if (addr < r.base || addr >= r.base + r.size) continue;
        if (!addrFilterIds_.empty() && addrFilterIds_.count(r.id) > 0)       return true;
        if (!addrFilterNames_.empty() && addrFilterNames_.count(r.name) > 0) return true;
    }
    return false;
}

bool
PortModuleStateGate::doInjection()
{
    triggered_ = {{false, false}};

    const PipelineStateBase* state =
        PipelineStateRegistry<PipelineStateBase>::get(stateKey_);
    if (!state) {
        // Agent hasn't published yet; no gate can match.
        return false;
    }
    if (!matchesState(*state)) {
        return false;
    }

    switch (mode_) {
        case Mode::Drop:
            triggered_[0] = (this->randFloat(0.0, 1.0) <= dropProb_);
            return triggered_[0];
        case Mode::Flip:
            triggered_[1] = (this->randFloat(0.0, 1.0) <= flipProb_);
            return triggered_[1];
        case Mode::DropFlip:
            triggered_[0] = (this->randFloat(0.0, 1.0) <= dropProb_);
            // Only roll for flip if we didn't already decide to drop the event;
            // a dropped event has nothing left to flip.
            triggered_[1] = !triggered_[0] &&
                            (this->randFloat(0.0, 1.0) <= flipProb_);
            return triggered_[0] || triggered_[1];
    }
    return false;
}

void
PortModuleStateGate::executeFaults(Event*& ev)
{
    // Per-event address filter runs AFTER doInjection() has already rolled the
    // dice. We intentionally consume the RNG draw even for filtered events so
    // that RNG traces remain stable regardless of which events the program
    // happens to send outside the protected region; only the side effect
    // (flipping bytes / cancelling delivery) is suppressed.
    if ((triggered_[0] || triggered_[1]) && !eventMatchesAddressFilter(ev)) {
        ++events_address_filtered_;
        triggered_ = {{false, false}};
        // Rate-limited visibility: emit a running status every 64 filtered
        // events so operators can see the address filter is live even when
        // the program never touches an MMIO address.
        if (logInjections_ && (events_address_filtered_ & 0x3F) == 0) {
            uint64_t addr = 0;
            if (auto* mem_ev = dynamic_cast<SST::MemHierarchy::MemEvent*>(ev)) {
                addr = mem_ev->getBaseAddr();
            }
            out_->output(
                "carcosa.PortModuleStateGate[%s] FILTER addr=0x%lx "
                "matched=%lu flipped=%lu dropped=%lu filtered=%lu\n",
                stateKey_.c_str(),
                static_cast<unsigned long>(addr),
                static_cast<unsigned long>(events_matched_),
                static_cast<unsigned long>(events_flipped_),
                static_cast<unsigned long>(events_dropped_),
                static_cast<unsigned long>(events_address_filtered_));
        }
        return;
    }

    if (triggered_[0]) {
        ++events_matched_;
        ++events_dropped_;
        // Grab MMIO address (if any) before cancelling delivery, so the log
        // line shows what we actually skipped.
        uint64_t addr = 0;
        if (auto* mem_ev = dynamic_cast<SST::MemHierarchy::MemEvent*>(ev)) {
            addr = mem_ev->getBaseAddr();
        }

        if (getInstallDirection() == installDirection::Receive) {
            this->cancelDelivery();
        } else {
#ifdef __SST_DEBUG_OUTPUT__
            dbg_->debug(CALL_INFO_LONG, 1, 0,
                        "PortModuleStateGate: drop requested in Send direction is a no-op "
                        "(the framework doesn't expose a cancel hook on Send).\n");
#endif
        }

        if (logInjections_) {
            out_->output(
                "carcosa.PortModuleStateGate[%s] DROP addr=0x%lx matched=%lu flipped=%lu dropped=%lu filtered=%lu\n",
                stateKey_.c_str(),
                static_cast<unsigned long>(addr),
                static_cast<unsigned long>(events_matched_),
                static_cast<unsigned long>(events_flipped_),
                static_cast<unsigned long>(events_dropped_),
                static_cast<unsigned long>(events_address_filtered_));
        }
        return;
    }

    if (triggered_[1]) {
        if (!fault[1]) {
            out_->fatal(CALL_INFO_LONG, -1,
                        "PortModuleStateGate: flip triggered but fault[1] is null "
                        "(fault_mode must be 'flip' or 'drop_flip' to enable flip).\n");
        }

        uint64_t addr = 0;
        size_t   payload_sz = 0;
        if (auto* mem_ev = dynamic_cast<SST::MemHierarchy::MemEvent*>(ev)) {
            addr       = mem_ev->getBaseAddr();
            payload_sz = mem_ev->getPayload().size();
        }

        fault[1]->faultLogic(ev);

        ++events_matched_;
        ++events_flipped_;

        if (logInjections_) {
            out_->output(
                "carcosa.PortModuleStateGate[%s] FLIP addr=0x%lx payload=%zu "
                "matched=%lu flipped=%lu dropped=%lu filtered=%lu\n",
                stateKey_.c_str(),
                static_cast<unsigned long>(addr),
                payload_sz,
                static_cast<unsigned long>(events_matched_),
                static_cast<unsigned long>(events_flipped_),
                static_cast<unsigned long>(events_dropped_),
                static_cast<unsigned long>(events_address_filtered_));
        }
    }
}
