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
