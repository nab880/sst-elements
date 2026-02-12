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

#ifndef SST_ELEMENTS_CARCOSA_FAULTINJECTORMEMH_H
#define SST_ELEMENTS_CARCOSA_FAULTINJECTORMEMH_H

#include <sst_config.h>
#include <random>
#include "sst/core/portModule.h"
#include "sst/core/event.h"
#include "sst/elements/memHierarchy/memEvent.h"
#include "sst/elements/carcosa/injectors/faultInjectorBase.h"
#include "sst/elements/carcosa/PMDataRegistry.h"
#include <map>
#include <utility>
#include <vector>

namespace SST::Carcosa {

// Enum to select basic fault injection logic or indicate a custom input
enum injectorLogic {
    StuckAt = 0,
    RandomFlip,
    RandomDrop,
    CorruptMemRegion,
    Custom
};

/**
 * PortModule for MemHierarchy fault injection with PMDataRegistry support.
 * Inherits from SST::PortModule directly; uses installDirection from FaultInjectorBase.
 */
class FaultInjectorMemH : public SST::PortModule
{
public:
    SST_ELI_REGISTER_PORTMODULE(
        FaultInjectorMemH,
        "carcosa",
        "faultInjectorMemH",
        SST_ELI_ELEMENT_VERSION(0, 1, 0),
        "Barebones PortModule used to connect fault injection logic to components"
    )

    SST_ELI_DOCUMENT_PARAMS(
        {"installDirection", "Flag which direction the injector should read from on a port. Valid optins are \'Send\', \'Receive\', and \'Both\'. Default is \'Receive\'."},
        {"injectionProbability", "The probability with which an injection should occur. Valid inputs range from 0 to 1. Default = 0.5."},
        {"faultType", "The type of fault to be injected. Options are stuckAt, randomFlip, randomDrop, corruptMemRegion, and custom."},
        {"stuckAtAddrs", "Map of addresses and bits that are stuck, along with the values of those stuck bits."},
        {"pmRegistryIds", "Comma-separated registry ids to listen to (e.g. 'default' or 'hali_0,hali_1'). Default 'default'."},
        {"pmId", "This PM's id for manager tracking. Optional."},
        {"debugManagerLogic", "If true, print [ManagerLogic] and PM-read debug messages. Default 0."}
    )

    FaultInjectorMemH(Params& params);

    FaultInjectorMemH() = default;
    ~FaultInjectorMemH() {}

    void eventSent(uintptr_t key, Event*& ev) override;
    void interceptHandler(uintptr_t key, Event*& data, bool& cancel) override;

    /** Call during init to push RegisterPM to all registries. Invoked by link if supported. */
    void init(unsigned phase);

    bool installOnReceive() override
    {
        switch (installDirection_) {
            case Send:
                return false;
            case Receive:
            //case Both:
            default:
                return true;
        }
    }
    bool installOnSend() override
    {
        switch (installDirection_) {
            case Send:
            //case Both:
                return true;
            case Receive:
            default:
                return false;
        }
    }

private:
    void (SST::Carcosa::FaultInjectorMemH::* faultLogic)(Event*&);
    std::random_device rd;
    std::default_random_engine generator;
    std::uniform_real_distribution<double> dist_fp;

    MemHierarchy::Command cmd;
    installDirection installDirection_ = installDirection::Receive;
    double injectionProbability_ = 0.5;

    // map of addr->{bit, value} for saving stuck bit values
    std::map<SST::MemHierarchy::Addr, std::vector<std::pair<int, bool>>> stuckAtMap;

    std::vector<std::string> pmRegistryIds_;
    std::vector<PMDataRegistry*> registries_;
    std::string pmId_;
    bool registerPMSent_ = false;
    bool debugManagerLogic_ = false;

    /** Resolve registry ids to pointers and push RegisterPM if not done yet. Call from init() or first event. */
    void ensureRegistriesResolved();

    void serialize_order(SST::Core::Serialization::serializer& ser) override
    {
        SST::PortModule::serialize_order(ser);
        // serialize parameters like `SST_SER(<param_member>)`
        SST_SER(installDirection_);
        SST_SER(injectionProbability_);
        SST_SER(stuckAtMap);
    }
    ImplementSerializable(SST::Carcosa::FaultInjectorMemH)

    /**
     * Read event payload and perform the following:
     *  - If stuckAtMap.at(addr) exists, compare all listed bits with payload value
     *  - If payload value does not match mapped value, add bit to flip mask
     *  - Once all stored bit values have been compared, use flip mask to modify address data
     */
    void stuckAtFault(Event*& ev);

    void stuckAtInit(SST::Params& params);

    void randomFlipFault(Event*& ev);

    void randomDropFault(Event*& ev);

    void corruptMemRegionFault(Event*& ev);

    void customFault(Event*& ev);
};

} // namespace SST::Carcosa

#endif // SST_ELEMENTS_CARCOSA_FAULTINJECTORMEMH_H
