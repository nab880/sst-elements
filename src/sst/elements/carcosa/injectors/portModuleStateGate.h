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

#ifndef SST_ELEMENTS_CARCOSA_PORTMODULESTATEGATE_H
#define SST_ELEMENTS_CARCOSA_PORTMODULESTATEGATE_H

#include "sst/elements/carcosa/injectors/faultInjectorBase.h"
#include "sst/elements/carcosa/components/pipelineStateRegistry.h"

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace SST::Carcosa {

/**
 * Generic state-gated fault injector.
 *
 * A PortModule that cancels or corrupts an event only when a user-configured
 * predicate against the PipelineStateRegistry<PipelineStateBase> snapshot
 * keyed by `state_key` matches.
 *
 * Drop path is handled inline via FaultInjectorBase::cancelDelivery() so the
 * gate works against arbitrary Event subtypes. Flip path delegates to
 * RandomFlipFault at fault[1] (inherently MemEvent-specific, since flipping
 * requires a payload); fault[0] is intentionally left null.
 *
 * This replaces the workload-specific "check currentKernel && vitLayer &&
 * tensor" gating pattern used in the VLA example with a composable predicate
 * list built from generic PipelineStateBase fields:
 *
 *   - `kernels`              : CSV of integer kernel ids; matches if
 *                              currentKernel is in the set.
 *   - `pipeline_cycle_start` /
 *     `pipeline_cycle_end`   : inclusive range; either may be omitted.
 *   - `region_ids`           : CSV of integer MemoryRegion ids; matches if
 *                              any valid region in regions[] has a matching id.
 *   - `region_names`         : CSV of MemoryRegion::name strings; matches
 *                              if any valid region has a matching name.
 *
 * All configured predicates are ANDed. A gate with no predicates configured
 * matches every event (i.e. degenerates to a plain DropFlipFaultInjector).
 *
 * Subclasses tailored to a specific workload (e.g. VLAFaultGate adding
 * vit_layers / prefill_layers / decode_layers) extend behavior by overriding
 * `buildPredicates(Params&)` to register extra lambdas or `matchesState(...)`
 * to replace the AND-of-predicates default entirely.
 */
class PortModuleStateGate : public FaultInjectorBase {
public:
    SST_ELI_REGISTER_PORTMODULE(
        PortModuleStateGate,
        "carcosa",
        "PortModuleStateGate",
        SST_ELI_ELEMENT_VERSION(0, 1, 0),
        "State-gated drop/flip fault injector. Consults PipelineStateRegistry "
        "before firing the wrapped fault."
    )

    SST_ELI_DOCUMENT_PARAMS(
        {"state_key",             "Required. Key into PipelineStateRegistry<PipelineStateBase>."},
        {"fault_mode",            "One of 'drop', 'flip', or 'drop_flip'. Default 'drop'."},
        {"drop_probability",      "Probability a matching event is dropped (used by 'drop'/'drop_flip'). Default 1.0."},
        {"flip_probability",      "Probability a matching event has a bit flipped (used by 'flip'/'drop_flip'). Default 1.0."},
        {"kernels",               "Optional CSV of kernel ids that enable the gate when currentKernel is in the set."},
        {"pipeline_cycle_start",  "Optional inclusive lower bound for pipelineCycle."},
        {"pipeline_cycle_end",    "Optional inclusive upper bound for pipelineCycle."},
        {"region_ids",            "Optional CSV of MemoryRegion ids; gate enables if any valid region id is in the set."},
        {"region_names",          "Optional CSV of MemoryRegion::name strings; gate enables if any valid region name matches."}
    )

    PortModuleStateGate(Params& params);
    PortModuleStateGate() = default;
    ~PortModuleStateGate() override = default;

protected:
    /**
     * Predicate signature used by the built-in predicate list.
     *
     * Runs against the latest PipelineStateBase snapshot. Predicates are
     * typically captured lambdas that own the configured parameter set
     * (e.g. a std::set<int> of allowed kernel ids).
     */
    using Predicate = std::function<bool(const PipelineStateBase&)>;

    enum class Mode { Drop, Flip, DropFlip };

    // Configuration
    std::string            stateKey_;
    Mode                   mode_ = Mode::Drop;
    double                 dropProb_ = 1.0;
    double                 flipProb_ = 1.0;

    // Composed predicates (AND semantics; empty list => always-match).
    std::vector<Predicate> predicates_;

    // drop_flip-style dual-trigger state, set in doInjection(), read in executeFaults().
    std::array<bool, 2>    triggered_ = {{false, false}};

    /**
     * Registers built-in predicates from params. Subclasses should call
     * PortModuleStateGate::buildPredicates(params) first, then push any
     * workload-specific predicates onto predicates_.
     */
    virtual void buildPredicates(Params& params);

    /**
     * Default predicate evaluation: returns true iff every entry in
     * predicates_ returns true for `state`. Override to replace the
     * AND-reduction with arbitrary logic.
     */
    virtual bool matchesState(const PipelineStateBase& state) const;

    bool doInjection() override;
    void executeFaults(Event*& ev) override;

    void serialize_order(SST::Core::Serialization::serializer& ser) override {
        FaultInjectorBase::serialize_order(ser);
        SST_SER(stateKey_);
        SST_SER(dropProb_);
        SST_SER(flipProb_);
        SST_SER(triggered_);
        // NOTE: predicates_ is a vector<std::function>; not serialized. It is
        // rebuilt from params in the ctor, which runs after deserialization
        // of base members.
    }
    ImplementVirtualSerializable(SST::Carcosa::PortModuleStateGate)

private:
    static Mode parseMode(const std::string& s);
};

} // namespace SST::Carcosa

#endif // SST_ELEMENTS_CARCOSA_PORTMODULESTATEGATE_H
