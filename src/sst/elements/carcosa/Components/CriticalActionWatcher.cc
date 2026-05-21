// Copyright 2009-2025 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2025, NTESS
// All rights reserved.

#include "sst_config.h"
#include "sst/elements/carcosa/Components/CriticalActionWatcher.h"
#include "sst/elements/carcosa/VLA-Example/Components/vla-fsm.h"
#include <algorithm>
#include <cinttypes>

using namespace SST;
using namespace SST::MemHierarchy;
using namespace SST::Carcosa;

namespace {

uint64_t fnv1a64(const uint8_t* data, size_t len) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace

CriticalActionWatcher::CriticalActionWatcher(ComponentId_t id, Params& params)
    : Component(id) {
    out_ = new Output("", 1, 0, Output::STDOUT);
    verbose_         = params.find<bool>("verbose", false);
    state_key_       = params.find<std::string>("state_key", "");
    critical_region_ = params.find<std::string>("critical_region", "action_queue");
    critical_len_    = params.find<uint64_t>("critical_len", 64);
    applyOnResponsesOnly_ =
        params.find<bool>("apply_on_responses_only", true);

    if (state_key_.empty()) {
        out_->fatal(CALL_INFO, -1,
            "CriticalActionWatcher '%s': state_key is required.\n",
            getName().c_str());
    }

    stat_frames_critical_corrupted_ =
        registerStatistic<uint64_t>("frames_critical_region_corrupted");

    if (isPortConnected("highlink")) {
        highlink_ = configureLink("highlink",
            new Event::Handler<CriticalActionWatcher, &CriticalActionWatcher::handleHighlink>(this));
    }
    if (isPortConnected("lowlink")) {
        lowlink_ = configureLink("lowlink",
            new Event::Handler<CriticalActionWatcher, &CriticalActionWatcher::handleLowlink>(this));
    }
    if (!highlink_ || !lowlink_) {
        out_->fatal(CALL_INFO, -1,
            "CriticalActionWatcher '%s': both highlink and lowlink must be connected.\n",
            getName().c_str());
    }
}

CriticalActionWatcher::~CriticalActionWatcher() {
    delete out_;
}

void CriticalActionWatcher::setup() {
    state_ptr_ = PipelineStateRegistry<PipelineStateBase>::getMutable(state_key_);
    if (!state_ptr_) {
        out_->fatal(CALL_INFO, -1,
            "CriticalActionWatcher '%s': no PipelineStateBase for key '%s'.\n",
            getName().c_str(), state_key_.c_str());
    }
}

void CriticalActionWatcher::init(unsigned phase) {
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

void CriticalActionWatcher::complete(unsigned phase) {
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

void CriticalActionWatcher::finish() {
    if (last_kernel_ == ACTUATE)
        finalizeActuateFrame();
    if (verbose_ && state_ptr_) {
        out_->output("CriticalActionWatcher '%s': frames_critical_region_corrupted=%" PRIu64 "\n",
                     getName().c_str(), state_ptr_->framesCriticalRegionCorrupted);
    }
}

bool CriticalActionWatcher::isResponseCmd(MemEvent* mev) const {
    if (!mev) return false;
    Command c = mev->getCmd();
    return c == Command::GetSResp || c == Command::GetXResp || c == Command::WriteResp;
}

bool CriticalActionWatcher::resolveCriticalBounds(uint64_t& base_out, uint64_t& len_out) const {
    base_out = 0;
    len_out  = 0;
    if (!state_ptr_) return false;
    for (const auto& r : state_ptr_->regions) {
        if (!r.valid || r.name != critical_region_) continue;
        base_out = r.base;
        len_out  = r.size;
        if (critical_len_ > 0 && critical_len_ < len_out)
            len_out = critical_len_;
        return len_out > 0;
    }
    return false;
}

bool CriticalActionWatcher::eventOverlapsCritical(MemEvent* mev,
                                                    uint64_t& rel_off,
                                                    uint64_t& overlap_len) const {
    rel_off = 0;
    overlap_len = 0;
    if (!mev) return false;
    uint64_t fbase = crit_base_, flen = crit_len_;
    if (fbase == 0 && flen == 0 && !resolveCriticalBounds(fbase, flen))
        return false;
    uint64_t vaddr = mev->getVirtualAddress();
    uint64_t addr  = (vaddr != 0) ? vaddr : mev->getAddr();
    uint64_t size  = mev->getPayload().empty() ? 64u : mev->getPayload().size();
    uint64_t end   = addr + size;
    uint64_t fend  = fbase + flen;
    if (!(addr < fend && end > fbase)) return false;
    uint64_t ostart = std::max(addr, fbase);
    uint64_t oend   = std::min(end, fend);
    rel_off      = ostart - fbase;
    overlap_len  = oend - ostart;
    return overlap_len > 0;
}

void CriticalActionWatcher::mergePayloadIntoSnapshot(uint64_t rel_off,
                                                     const std::vector<uint8_t>& payload) {
    if (snapshot_.empty()) return;
    for (size_t i = 0; i < payload.size(); ++i) {
        uint64_t pos = rel_off + i;
        if (pos >= snapshot_.size()) break;
        uint8_t b = payload[i];
        if (snapshot_[pos] != b) snapshot_dirty_ = true;
        snapshot_[pos] = b;
    }
}

uint64_t CriticalActionWatcher::hashSnapshot() const {
    if (snapshot_.empty()) return 0;
    return fnv1a64(snapshot_.data(), snapshot_.size());
}

void CriticalActionWatcher::finalizeActuateFrame() {
    if (!state_ptr_) return;
    state_ptr_->watcherActionChecksum =
        hashSnapshot();
    state_ptr_->watcherActionChecksumValid = !snapshot_.empty();
    state_ptr_->watcherCriticalCorrupted   = snapshot_dirty_;
    if (snapshot_dirty_) {
        ++state_ptr_->framesCriticalRegionCorrupted;
        if (stat_frames_critical_corrupted_)
            stat_frames_critical_corrupted_->addData(1);
    }
    snapshot_.assign(crit_len_, 0);
    baseline_snapshot_.clear();
    snapshot_dirty_ = false;
}

void CriticalActionWatcher::handleHighlink(Event* ev) {
    auto* mev = dynamic_cast<MemEvent*>(ev);
    if (mev && state_ptr_) {
        if (!resolveCriticalBounds(crit_base_, crit_len_)) {
            crit_base_ = 0;
            crit_len_  = 0;
        } else if (snapshot_.size() != crit_len_) {
            snapshot_.assign(crit_len_, 0);
        }

        int k = state_ptr_->currentKernel;
        if (last_kernel_ == ACTUATE && k != ACTUATE)
            finalizeActuateFrame();
        last_kernel_ = k;

        if ((!applyOnResponsesOnly_ || isResponseCmd(mev)) && k == ACTUATE) {
            uint64_t rel = 0, olen = 0;
            if (eventOverlapsCritical(mev, rel, olen)) {
                const auto& payload = mev->getPayload();
                if (!payload.empty())
                    mergePayloadIntoSnapshot(rel, payload);
            }
        }
    }
    if (lowlink_) lowlink_->send(ev);
}

void CriticalActionWatcher::handleLowlink(Event* ev) {
    if (highlink_) highlink_->send(ev);
}
