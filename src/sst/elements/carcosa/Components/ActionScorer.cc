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
#include "sst/elements/carcosa/Components/ActionScorer.h"
#include "sst/elements/carcosa/Components/PipelineStateRegistry.h"
#include "sst/elements/carcosa/VLA-Example/Components/vla-fsm.h"
#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

using namespace SST;
using namespace SST::Carcosa;

ActionScorer::ActionScorer(ComponentId_t id, Params& params)
    : Component(id) {
    out_ = new Output("", 1, 0, Output::STDOUT);
    state_key_       = params.find<std::string>("state_key", "");
    golden_path_     = params.find<std::string>("golden_log", "");
    emit_golden_     = params.find<bool>("emit_golden", false);
    golden_required_ = params.find<bool>("golden_required", true);
    verbose_         = params.find<bool>("verbose", false);
    if (state_key_.empty()) {
        out_->fatal(CALL_INFO, -1,
            "ActionScorer '%s': state_key is required.\n", getName().c_str());
    }
    stat_frames_total_       = registerStatistic<uint64_t>("frames_total");
    stat_frames_dropped_     = registerStatistic<uint64_t>("frames_dropped");
    stat_frames_argmax_diff_ = registerStatistic<uint64_t>("frames_argmax_diff");
    stat_frames_unsafe_      = registerStatistic<uint64_t>("frames_safety_violated");
    stat_frames_o1_          = registerStatistic<uint64_t>("frames_outcome_O1");
    stat_frames_o2_          = registerStatistic<uint64_t>("frames_outcome_O2");
    stat_frames_o3_          = registerStatistic<uint64_t>("frames_outcome_O3");
    stat_frames_o4_          = registerStatistic<uint64_t>("frames_outcome_O4");
}

const char* ActionScorer::outcomeClassLabel(int oclass) {
    switch (oclass) {
    case 1: return "O1";
    case 2: return "O2";
    case 3: return "O3";
    case 4: return "O4";
    default: return "O?";
    }
}

ActionScorer::~ActionScorer() {
    delete out_;
}

void ActionScorer::loadGoldenLog() {
    if (golden_loaded_) return;
    golden_loaded_ = true;
    if (golden_path_.empty()) return;

    std::ifstream f(golden_path_);
    if (!f.is_open()) {
        if (golden_required_) {
            out_->fatal(CALL_INFO, -1,
                "ActionScorer '%s': golden_log='%s' could not be opened and "
                "golden_required=true. Refusing to proceed with a degraded "
                "score that would report unsafe_action_rate=0 for every "
                "frame regardless of BER. Either run the matching BER=0 "
                "Phase 1 pass to emit the golden file, or set "
                "golden_required=false to opt in to the legacy passthrough.\n",
                getName().c_str(), golden_path_.c_str());
        }
        out_->output("ActionScorer '%s': WARNING golden_log '%s' could not be "
                     "opened; golden_required=false, so every frame will "
                     "report argmax_changed=0. unsafe_action_rate will reflect "
                     "drop_rate only.\n",
                     getName().c_str(), golden_path_.c_str());
        return;
    }
    std::string line;
    int line_no = 0;
    while (std::getline(f, line)) {
        ++line_no;
        if (line.empty()) continue;
        if (line[0] == '#') continue;
        // Allow header line "pipeline_cycle,kernel_at_close,action_checksum".
        if (line.find("pipeline_cycle") != std::string::npos) continue;

        std::stringstream ss(line);
        std::string cell;
        std::vector<std::string> parts;
        while (std::getline(ss, cell, ',')) parts.push_back(cell);
        if (parts.size() < 3) continue;

        GoldenEntry e{};
        try {
            e.pipelineCycle = std::stoi(parts[0]);
            e.kernelAtClose = std::stoi(parts[1]);
            e.checksum      = std::stoull(parts[2]);
        } catch (...) {
            out_->output("ActionScorer '%s': skipping malformed golden_log line %d: '%s'\n",
                         getName().c_str(), line_no, line.c_str());
            continue;
        }
        golden_.push_back(e);
    }
    if (verbose_) {
        out_->output("ActionScorer '%s': loaded %zu golden entries from '%s'\n",
                     getName().c_str(), golden_.size(), golden_path_.c_str());
    }
    if (golden_.empty() && golden_required_) {
        out_->fatal(CALL_INFO, -1,
            "ActionScorer '%s': golden_log='%s' opened but contained zero "
            "valid entries (header-only or all rows malformed) and "
            "golden_required=true. Refusing to score with an empty golden "
            "table; re-run the BER=0 emit-golden pass.\n",
            getName().c_str(), golden_path_.c_str());
    }
}

void ActionScorer::setup() {
    loadGoldenLog();
}

void ActionScorer::finish() {
    const PipelineStateBase* s =
        PipelineStateRegistry<PipelineStateBase>::get(state_key_);
    if (!s) {
        out_->output("ActionScorer '%s': no snapshot for key '%s'; nothing to score.\n",
                     getName().c_str(), state_key_.c_str());
        return;
    }

    // Index golden by (pipelineCycle, kernelAtClose) -> checksum so the lookup
    // tolerates frames being scored out of order across multi-cycle runs.
    std::map<std::pair<int,int>, uint64_t> golden_idx;
    for (const auto& g : golden_) {
        golden_idx[{g.pipelineCycle, g.kernelAtClose}] = g.checksum;
    }

    out_->output("\n=== Action Scorer %s Per-Frame Trace ===\n", getName().c_str());
    out_->output("pipeline_cycle,kernel_at_close,kernel_name,"
                 "attributing_kernel_id,attributing_kernel_name,dropped,"
                 "escapes_in_frame,flips_in_frame,action_checksum,"
                 "golden_checksum,argmax_changed,safety_violated,outcome_class,"
                 "sim_time_ps\n");

    uint64_t prev_escapes = 0;
    uint64_t prev_flips   = 0;
    uint64_t total        = 0;
    uint64_t dropped      = 0;
    uint64_t argmax_diff  = 0;
    uint64_t unsafe       = 0;
    uint64_t o1 = 0, o2 = 0, o3 = 0, o4 = 0;

    std::ostringstream golden_emit;
    if (emit_golden_) {
        golden_emit << "pipeline_cycle,kernel_at_close,action_checksum\n";
    }

    for (const auto& fr : s->frames) {
        uint64_t escapes_in = fr.cumulativeEscapes >= prev_escapes
                                ? fr.cumulativeEscapes - prev_escapes : 0;
        uint64_t flips_in   = fr.cumulativeFlips   >= prev_flips
                                ? fr.cumulativeFlips   - prev_flips   : 0;
        prev_escapes = fr.cumulativeEscapes;
        prev_flips   = fr.cumulativeFlips;

        uint64_t golden_cs = 0;
        bool has_golden = false;
        auto it = golden_idx.find({fr.pipelineCycle, fr.kernelAtClose});
        if (it != golden_idx.end()) {
            golden_cs = it->second;
            has_golden = true;
        }

        bool argmax_changed = has_golden && (golden_cs != fr.actionChecksum);
        bool had_escape     = escapes_in > 0;
        // Outcome taxonomy uses drops/argmax/escape; unsafe_action_rate is
        // reserved for silent corruption (escape or argmax), not DUE drops.
        bool safety_violated = fr.dropped || argmax_changed || had_escape;
        bool unsafe_action   = argmax_changed || had_escape;

        int outcome_class = 1;
        if (safety_violated) {
            if (fr.dropped && !argmax_changed) outcome_class = 2;
            else if (argmax_changed)           outcome_class = 3;
            else if (had_escape)               outcome_class = 4;
            else                               outcome_class = 2;
        }

        const char* kname = (fr.kernelAtClose >= 0 && fr.kernelAtClose < NUM_STATES)
                                ? vlaStateName(fr.kernelAtClose)
                                : "UNKNOWN";

        // Tier B (Fig. 3a): attributing_kernel = the kernel with the most
        // EccGuard escapes during this frame. Falls back to kernelAtClose
        // when the simulator didn't see any escapes (low-BER cells).
        int attr_id = fr.attributingKernel;
        if (attr_id < 0) attr_id = fr.kernelAtClose;
        const char* aname = (attr_id >= 0 && attr_id < NUM_STATES)
                                ? vlaStateName(attr_id)
                                : "UNKNOWN";

        out_->output("%d,%d,%s,%d,%s,%d,%" PRIu64 ",%" PRIu64
                     ",%" PRIu64 ",%" PRIu64 ",%d,%d,%s,%" PRIu64 "\n",
                     fr.pipelineCycle, fr.kernelAtClose, kname,
                     attr_id, aname,
                     fr.dropped ? 1 : 0,
                     escapes_in, flips_in,
                     fr.actionChecksum, golden_cs,
                     argmax_changed ? 1 : 0,
                     safety_violated ? 1 : 0,
                     outcomeClassLabel(outcome_class),
                     fr.simTimePs);

        if (emit_golden_) {
            golden_emit << fr.pipelineCycle << ","
                        << fr.kernelAtClose << ","
                        << fr.actionChecksum << "\n";
        }

        ++total;
        if (fr.dropped)        ++dropped;
        if (argmax_changed)    ++argmax_diff;
        if (unsafe_action)     ++unsafe;
        switch (outcome_class) {
        case 1: ++o1; break;
        case 2: ++o2; break;
        case 3: ++o3; break;
        case 4: ++o4; break;
        default: break;
        }
    }
    out_->output("=== End Action Scorer %s Per-Frame Trace (%" PRIu64 " frames) ===\n\n",
                 getName().c_str(), total);

    if (stat_frames_total_)       stat_frames_total_->addData(total);
    if (stat_frames_dropped_)     stat_frames_dropped_->addData(dropped);
    if (stat_frames_argmax_diff_) stat_frames_argmax_diff_->addData(argmax_diff);
    if (stat_frames_unsafe_)      stat_frames_unsafe_->addData(unsafe);
    if (stat_frames_o1_)          stat_frames_o1_->addData(o1);
    if (stat_frames_o2_)          stat_frames_o2_->addData(o2);
    if (stat_frames_o3_)          stat_frames_o3_->addData(o3);
    if (stat_frames_o4_)          stat_frames_o4_->addData(o4);

    out_->output("=== Action Scorer %s Summary ===\n", getName().c_str());
    out_->output("frames_total,frames_dropped,frames_argmax_diff,frames_unsafe,"
                 "frames_outcome_O1,frames_outcome_O2,frames_outcome_O3,frames_outcome_O4,"
                 "drop_rate,argmax_change_rate,unsafe_action_rate\n");
    double dr = total ? static_cast<double>(dropped)     / total : 0.0;
    double ar = total ? static_cast<double>(argmax_diff) / total : 0.0;
    double ur = total ? static_cast<double>(unsafe)      / total : 0.0;
    out_->output("%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%"
                 PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.6e,%.6e,%.6e\n",
                 total, dropped, argmax_diff, unsafe, o1, o2, o3, o4, dr, ar, ur);
    out_->output("=== End Action Scorer %s Summary ===\n\n", getName().c_str());

    if (emit_golden_) {
        out_->output("=== Action Scorer %s Golden Emit ===\n", getName().c_str());
        out_->output("%s", golden_emit.str().c_str());
        out_->output("=== End Action Scorer %s Golden Emit ===\n\n", getName().c_str());
    }
}
