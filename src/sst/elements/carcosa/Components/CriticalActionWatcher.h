// Copyright 2009-2025 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2025, NTESS
// All rights reserved.

#ifndef SST_ELEMENTS_CARCOSA_CRITICAL_ACTION_WATCHER_H
#define SST_ELEMENTS_CARCOSA_CRITICAL_ACTION_WATCHER_H

#include "sst/elements/carcosa/Components/PipelineStateRegistry.h"
#include "sst/elements/memHierarchy/memEvent.h"
#include <sst/core/component.h>
#include <sst/core/link.h>
#include <sst/core/output.h>
#include <cstdint>
#include <string>
#include <vector>

namespace SST {
namespace Carcosa {

/**
 * Inline memory observer that snapshots CPU-observed bytes in a labeled DRAM
 * region (typically action_queue) and publishes a per-frame checksum into
 * PipelineStateBase for ActionScorer.
 */
class CriticalActionWatcher : public SST::Component {
public:
    SST_ELI_REGISTER_COMPONENT(
        CriticalActionWatcher,
        "Carcosa",
        "CriticalActionWatcher",
        SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "Snapshots CPU-observed bytes in a critical memory region and stamps "
        "FrameRecord.actionChecksum at ACTUATE frame close.",
        COMPONENT_CATEGORY_MEMORY)

    SST_ELI_DOCUMENT_PARAMS(
        {"verbose",            "Enable verbose output.", "false"},
        {"state_key",          "PipelineStateRegistry key (required).", ""},
        {"critical_region",    "Published region name to watch (e.g. action_queue).", "action_queue"},
        {"critical_len",       "Max bytes to hash (0 = entire region).", "64"},
        {"apply_on_responses_only", "Only snapshot read responses.", "true"},
        {"actuation_kernel",   "Workload-supplied kernel name that marks frame commitment. The snapshot is taken during this kernel and frozen on the trailing edge (kernel != actuation_kernel). Falls back to PipelineStateBase::actuationKernelName when this param is empty.", ""})

    SST_ELI_DOCUMENT_PORTS(
        {"highlink", "Toward directory/cache", {"memHierarchy.MemEventBase"}},
        {"lowlink",  "Toward EccGuard/memory", {"memHierarchy.MemEventBase"}})

    SST_ELI_DOCUMENT_STATISTICS(
        {"frames_critical_region_corrupted", "Frames with any changed byte in the critical window.", "count", 1})

    CriticalActionWatcher(SST::ComponentId_t id, SST::Params& params);
    ~CriticalActionWatcher() override;

    void setup() override;
    void init(unsigned phase) override;
    void complete(unsigned phase) override;
    void finish() override;

private:
    void handleHighlink(SST::Event* ev);
    void handleLowlink(SST::Event* ev);

    bool isResponseCmd(SST::MemHierarchy::MemEvent* mev) const;
    bool resolveCriticalBounds(uint64_t& base_out, uint64_t& len_out) const;
    bool eventOverlapsCritical(SST::MemHierarchy::MemEvent* mev,
                               uint64_t& rel_off, uint64_t& overlap_len) const;
    void mergePayloadIntoSnapshot(uint64_t rel_off, const std::vector<uint8_t>& payload);
    uint64_t hashSnapshot() const;
    void finalizeActuateFrame();

    SST::Output* out_      = nullptr;
    bool         verbose_  = false;
    std::string  state_key_;
    std::string  critical_region_;
    uint64_t     critical_len_ = 64;

    bool applyOnResponsesOnly_ = true;

    PipelineStateBase* state_ptr_ = nullptr;
    std::string        actuation_kernel_name_;
    std::string        last_kernel_name_;
    bool               saw_kernel_ = false;

    uint64_t crit_base_ = 0;
    uint64_t crit_len_  = 0;
    std::vector<uint8_t> snapshot_;
    std::vector<uint8_t> baseline_snapshot_;
    bool snapshot_dirty_ = false;

    SST::Link* highlink_ = nullptr;
    SST::Link* lowlink_  = nullptr;

    Statistics::Statistic<uint64_t>* stat_frames_critical_corrupted_ = nullptr;
};

} // namespace Carcosa
} // namespace SST

#endif /* SST_ELEMENTS_CARCOSA_CRITICAL_ACTION_WATCHER_H */
