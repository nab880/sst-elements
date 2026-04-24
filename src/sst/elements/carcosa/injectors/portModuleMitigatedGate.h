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

#ifndef SST_ELEMENTS_CARCOSA_PORTMODULE_MITIGATED_GATE_H
#define SST_ELEMENTS_CARCOSA_PORTMODULE_MITIGATED_GATE_H

#include "sst/elements/carcosa/injectors/portModuleStateGate.h"

#include <cstdint>
#include <string>

namespace SST::Carcosa {

/**
 * PortModuleMitigatedGate - an analytic mitigation model layered on top of
 * the PortModuleStateGate decision.
 *
 * Parameter set = parent's phase/region predicates (kernels, region_names,
 * pipeline_cycle_start/end, etc.) + a single additional `mitigation_scheme`
 * selector + its per-scheme cost/mask parameters.
 *
 * When the parent gate would have triggered a fault, this subclass first
 * rolls a scheme-specific "mask" Bernoulli:
 *   - `off`               : pass-through, no mitigation, no overhead
 *   - `secded`            : mask_probability 1.0 (single-bit always
 *                           correctable); charges `ecc_check_cycles`
 *                           per matched event.
 *   - `dmr`               : mask_probability 0.999; charges
 *                           `dmr_compute_cycles` (2x compute + vote).
 *   - `checkpoint`        : mask_probability 0.0 (does not prevent SDC);
 *                           charges `checkpoint_rollback_cycles` on DUE
 *                           paths (approximated here as every matched
 *                           event, callers should gate checkpoint to the
 *                           DUE-producing injectors).
 *   - `selective_secded`  : identical to `secded` but the gate predicates
 *                           (inherited from the parent) decide which
 *                           events are actually protected; unmatched
 *                           events bypass the scheme and get neither
 *                           masking nor overhead.
 *
 * The subclass exposes three running counters useful for the Claim 4
 * mitigation Pareto sweep:
 *   - events_matched_   : events that activated the scheme (parent gate matched)
 *   - events_masked_    : subset that were masked by the scheme
 *   - cycles_overhead_  : sum of per-event cycle overheads for matched events
 *
 * Counters are reported via Output at the end of simulation. The
 * carcosa-paper campaign driver scrapes these from the SST log and stores
 * them in the fault-spec v1 `cycles_overhead` column.
 */
class PortModuleMitigatedGate : public PortModuleStateGate {
public:
    SST_ELI_REGISTER_PORTMODULE(
        PortModuleMitigatedGate,
        "carcosa",
        "PortModuleMitigatedGate",
        SST_ELI_ELEMENT_VERSION(0, 1, 0),
        "Mitigation-aware extension of PortModuleStateGate. Wraps the gate "
        "decision with a scheme-specific mask/overhead model for Claim 4 "
        "of the two-oracle VLA fault-injection paper."
    )

    SST_ELI_DOCUMENT_PARAMS(
        {"mitigation_scheme",
         "One of 'off' | 'secded' | 'dmr' | 'checkpoint' | 'selective_secded'. "
         "Default 'off' (no mitigation, no overhead)."},
        {"mask_probability",
         "Override for the scheme-default Bernoulli mask probability. "
         "-1 means 'use the scheme default'. Default -1."},
        {"ecc_check_cycles",
         "Per-matched-event overhead charged when scheme in {secded, "
         "selective_secded}. Default 2."},
        {"dmr_compute_cycles",
         "Per-matched-event overhead charged when scheme == dmr. Default 100."},
        {"checkpoint_rollback_cycles",
         "Per-matched-event overhead charged when scheme == checkpoint. "
         "Default 10000."}
    )

    PortModuleMitigatedGate(Params& params);
    PortModuleMitigatedGate() = default;
    ~PortModuleMitigatedGate() override;

    enum class Scheme { Off, Secded, Dmr, Checkpoint, SelectiveSecded };

protected:
    /**
     * Applies the parent gate predicate first, and if it would fire a fault,
     * rolls the mitigation scheme's mask Bernoulli. A successful mask clears
     * the triggered flags so executeFaults() is a no-op; either way the
     * scheme's cycle overhead is charged to cycles_overhead_.
     */
    bool doInjection() override;

    void serialize_order(SST::Core::Serialization::serializer& ser) override {
        PortModuleStateGate::serialize_order(ser);
        // Counters are runtime state; scheme is reconstructed from params.
        SST_SER(events_matched_);
        SST_SER(events_masked_);
        SST_SER(cycles_overhead_);
    }
    ImplementVirtualSerializable(SST::Carcosa::PortModuleMitigatedGate)

private:
    static Scheme parseScheme(const std::string& s);
    double        schemeDefaultMaskProb() const;
    uint64_t      schemeCyclesPerEvent() const;

    Scheme   scheme_          = Scheme::Off;
    double   maskProbability_ = -1.0;   // -1 => schemeDefaultMaskProb()

    uint64_t eccCheckCycles_          = 2;
    uint64_t dmrComputeCycles_        = 100;
    uint64_t checkpointRollbackCycles_ = 10000;

    uint64_t events_matched_  = 0;
    uint64_t events_masked_   = 0;
    uint64_t cycles_overhead_ = 0;
};

} // namespace SST::Carcosa

#endif // SST_ELEMENTS_CARCOSA_PORTMODULE_MITIGATED_GATE_H
