// Copyright 2009-2025 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2025, NTESS
// All rights reserved.
//
// Portions are copyright of other developers:
// See the file CONTRIBUTORS.TXT in the top level directory of the distribution.
//
// This file is part of the SST software package. For license information,
// see the LICENSE file in the top level directory of the distribution.

#ifndef SST_ELEMENTS_CARCOSA_PIPELINE_STATE_REGISTRY_H
#define SST_ELEMENTS_CARCOSA_PIPELINE_STATE_REGISTRY_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

namespace SST {
namespace Carcosa {

/**
 * Labeled memory range published by an InterceptionAgent so that fault
 * injectors (PortModules) can gate on address-in-region predicates.
 *
 * The containing PipelineStateBase::regions vector treats the slot index as
 * the region id; MemoryRegion::id is provided as a convenience for callers
 * that prefer to store the id in the entry itself.
 */
struct MemoryRegion {
    uint64_t    base  = 0;
    uint64_t    size  = 0;
    bool        valid = false;
    int         id    = -1;
    /**
     * Optional symbolic label (e.g. "weights", "kv_cache", "action_queue").
     * PortModuleStateGate exposes a `region_names` predicate that matches
     * against this field so configs don't have to hard-code slot ids.
     */
    std::string name;
};

/**
 * Base snapshot published by an InterceptionAgent on every FSM transition and
 * consumed by PortModule fault injectors.
 *
 * Every pipeline agent publishes at minimum:
 *   - currentKernel  : the FSM state / kernel id currently executing
 *   - pipelineCycle  : a monotonically increasing count of full pipeline iterations
 *   - regions[]      : labeled memory ranges (tensors, queues, ring buffers, ...)
 *
 * Workload-specific snapshots (e.g. VLAFaultState with vitLayer/prefillLayer/
 * decodeLayer/actionChecksum/goldenChecksum) derive from this struct and are
 * looked up through PipelineStateRegistry<Derived>.
 *
 * The staged-base/staged-size slots support the standard multi-MMIO-write
 * publish protocol: the workload writes base_lo/base_hi/size, then commits
 * with the region id; see commitStagedRegion().
 */
struct PipelineStateBase {
    int      currentKernel = -1;
    int      pipelineCycle = 0;

    uint64_t stagedBase = 0;
    uint64_t stagedSize = 0;

    std::vector<MemoryRegion> regions;

    /**
     * Set by EccGuard when a Detectable-Uncorrectable Error is reported and
     * the guard is configured with due_action='drop_frame'. Pipeline agents
     * (VLAAgent / VLA*DelayAgent) consult this on every status-write and, if
     * set, fast-forward the FSM to ACTUATE, increment framesDropped, then
     * clear the flag for the next frame.
     */
    bool     frameAbortRequested = false;

    /** Cumulative count of pipeline cycles that were aborted due to DUE. */
    int      framesDropped = 0;

    /**
     * Per-frame behavioral record. Pushed by the VLA pipeline agent on every
     * completed pipeline cycle (ACTUATE -> IDLE transition). Consumed by
     * Carcosa.ActionScorer for end-task safety metrics.
     *
     * actionChecksum: workload-defined fingerprint of the actuator output
     * (e.g. argmax token concat-XOR with a hash of the action vector). Used
     * to detect SDC-induced action divergence relative to a golden run.
     *
     * cumulativeEscapes: snapshot of EccGuard.events_escape at frame close
     * (read out of the registry's running counter; agents copy
     * eccCumulativeEscapes into the frame).
     */
    struct FrameRecord {
        int      pipelineCycle      = 0;
        int      kernelAtClose      = -1;
        // Tier B (Fig. 3a) violation attribution. Set by the VLA pipeline
        // agent at frame close to the kernel that produced the largest
        // number of EccGuard escape classifications during this frame.
        // Populated only when the agent has read the per-frame escape map
        // EccGuard publishes (eccPerFrameEscapesByKernel below). Falls
        // back to kernelAtClose (== ACTUATE on the synthetic FSM) when no
        // escapes were observed in the frame.
        int      attributingKernel  = -1;
        bool     dropped            = false;
        uint64_t actionChecksum     = 0;
        uint64_t cumulativeEscapes  = 0;
        uint64_t cumulativeFlips    = 0;
        uint64_t simTimePs          = 0;
    };
    std::vector<FrameRecord> frames;

    /**
     * Per-frame per-kernel escape counter, updated by EccGuard whenever
     * applyPolicy classifies an event as SilentEscape. Indexed by kernel
     * id with a final slot (NUM_STATES) for "no FSM publisher / unknown
     * kernel" so the array is bounds-safe even when EccGuard sees traffic
     * before any kernel is published.
     *
     * The VLA pipeline agent reads this map at frame close, picks the
     * argmax, writes the result into FrameRecord.attributingKernel, then
     * resets the map for the next frame. The reset step is intentionally
     * driven by the consumer rather than EccGuard so a single guard can
     * serve multiple agents without race conditions on the per-frame
     * edge.
     *
     * A frame that closes with zero escapes (every slot still zero
     * after reset by the previous close) gets argmax==-1; the agent
     * then stamps attributingKernel = kernelAtClose so downstream
     * consumers always see a valid kernel id, and Fig. 3a's caption
     * disambiguates the fallback case.
     *
     * Allocated lazily by EccGuard on first escape.
     */
    std::vector<uint64_t> eccPerFrameEscapesByKernel;

    /**
     * Helper: argmax over eccPerFrameEscapesByKernel. Returns -1 when
     * the vector is empty or every slot is zero (no escapes since the
     * last reset). The "unknown" slot at index NUM_STATES is returned as
     * NUM_STATES so the caller can distinguish "argmax was the unlabeled
     * bucket" from "no signal at all" (-1).
     */
    int argmaxEccPerFrameEscapesByKernel() const {
        if (eccPerFrameEscapesByKernel.empty()) return -1;
        int      best_i = -1;
        uint64_t best_v = 0;
        for (size_t i = 0; i < eccPerFrameEscapesByKernel.size(); ++i) {
            if (eccPerFrameEscapesByKernel[i] > best_v) {
                best_v = eccPerFrameEscapesByKernel[i];
                best_i = static_cast<int>(i);
            }
        }
        return best_i;
    }

    /** Helper: zero out the per-frame escape map (consumer at frame close). */
    void resetEccPerFrameEscapesByKernel() {
        std::fill(eccPerFrameEscapesByKernel.begin(),
                  eccPerFrameEscapesByKernel.end(), 0u);
    }

    /**
     * Cumulative bit-flip / escape counters published by EccGuard so the
     * scorer / agents can compute per-frame deltas without reaching into
     * the guard's private state.
     */
    uint64_t eccCumulativeEscapes = 0;
    uint64_t eccCumulativeFlips   = 0;

    /**
     * Populated by CriticalActionWatcher at the end of each ACTUATE phase.
     * VLACpuDelayAgent / VLAAgent prefer this over workload MMIO when valid.
     */
    uint64_t watcherActionChecksum     = 0;
    bool     watcherActionChecksumValid = false;
    /** True if any CPU-observed byte in the critical window differed this frame. */
    bool     watcherCriticalCorrupted  = false;
    /** Per-run count of frames where watcherCriticalCorrupted was set (finish stat). */
    uint64_t framesCriticalRegionCorrupted = 0;

    /** Returns the region id (== slot index) whose range contains addr, or -1. */
    int regionIdForAddress(uint64_t addr) const {
        for (size_t i = 0; i < regions.size(); ++i) {
            const MemoryRegion& r = regions[i];
            if (r.valid && addr >= r.base && addr < r.base + r.size)
                return static_cast<int>(i);
        }
        return -1;
    }

    /** Grows the region table so that slot `id` is valid (filling intermediate slots). */
    void ensureRegionSlot(size_t id) {
        if (regions.size() <= id) {
            size_t old = regions.size();
            regions.resize(id + 1);
            for (size_t i = old; i <= id; ++i) regions[i].id = static_cast<int>(i);
        }
    }

    /**
     * Promotes the staged base/size into regions[id] and clears the staged slots.
     * Matches the HYADES_REGION_* / HYADES_TENSOR_* MMIO publish ABI.
     */
    void commitStagedRegion(size_t id) {
        ensureRegionSlot(id);
        regions[id].base  = stagedBase;
        regions[id].size  = stagedSize;
        regions[id].valid = stagedSize > 0;
        regions[id].id    = static_cast<int>(id);
        stagedBase = 0;
        stagedSize = 0;
    }

    virtual ~PipelineStateBase() = default;
};

/**
 * Host-side, string-keyed rendezvous for pipeline state snapshots.
 *
 * Agents call PipelineStateRegistry<T>::getOrCreate(key) in agentSetup() and
 * write into the returned snapshot on every FSM transition; PortModule fault
 * injectors call PipelineStateRegistry<T>::get(key) lazily (their constructor
 * runs before the agent's agentSetup()) and read the snapshot per event.
 *
 * One map is instantiated per T; this lets different workloads coexist in the
 * same simulation without key collisions (e.g. a VLA agent publishing
 * VLAFaultState alongside a different pipeline publishing its own subclass).
 */
template <typename T = PipelineStateBase>
class PipelineStateRegistry {
    static_assert(std::is_base_of<PipelineStateBase, T>::value ||
                  std::is_same<T, PipelineStateBase>::value,
                  "PipelineStateRegistry<T>: T must derive from PipelineStateBase");

public:
    /** Returns a pointer to the snapshot for `key`, creating a default-constructed entry if absent. */
    static T* getOrCreate(const std::string& key) {
        auto& m = map_();
        auto it = m.find(key);
        if (it == m.end()) it = m.emplace(key, T{}).first;
        return &it->second;
    }

    /** Read-only lookup; returns nullptr if no entry exists for `key`. */
    static const T* get(const std::string& key) {
        const auto& m = map_();
        auto it = m.find(key);
        return it == m.end() ? nullptr : &it->second;
    }

    /** Mutable lookup without insertion; returns nullptr if no entry exists for `key`. */
    static T* getMutable(const std::string& key) {
        auto& m = map_();
        auto it = m.find(key);
        return it == m.end() ? nullptr : &it->second;
    }

    static void   clear() { map_().clear(); }
    static size_t size()  { return map_().size(); }

private:
    static std::map<std::string, T>& map_() {
        static std::map<std::string, T> m;
        return m;
    }
};

} // namespace Carcosa
} // namespace SST

#endif /* SST_ELEMENTS_CARCOSA_PIPELINE_STATE_REGISTRY_H */
