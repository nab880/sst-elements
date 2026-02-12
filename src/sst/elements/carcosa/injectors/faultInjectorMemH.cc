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

#include "sst/elements/carcosa/injectors/faultInjectorMemH.h"
#include "sst/elements/carcosa/Components/PMDataRegistry.h"
#include <sstream>

using namespace SST::Carcosa;

namespace {
// Parse comma-separated registry id list; trim spaces; use "default" if empty.
std::vector<std::string> splitComma(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string id;
    while (std::getline(iss, id, ',')) {
        while (!id.empty() && id.front() == ' ') id.erase(0, 1);
        while (!id.empty() && id.back() == ' ') id.pop_back();
        if (!id.empty()) out.push_back(id);
    }
    if (out.empty()) out.push_back("default");
    return out;
}
}

namespace {

// Map of command string names to MemHierarchy::Command enum values
const std::map<std::string, SST::MemHierarchy::Command> commandMap = {
    {"GetS",         SST::MemHierarchy::Command::GetS},
    {"GetX",         SST::MemHierarchy::Command::GetX},
    {"Write",        SST::MemHierarchy::Command::Write},
    {"GetSX",        SST::MemHierarchy::Command::GetSX},
    {"FlushLine",    SST::MemHierarchy::Command::FlushLine},
    {"FlushLineInv", SST::MemHierarchy::Command::FlushLineInv},
    {"FlushAll",     SST::MemHierarchy::Command::FlushAll},
    {"GetSResp",     SST::MemHierarchy::Command::GetSResp},
    {"WriteResp",    SST::MemHierarchy::Command::WriteResp},
    {"GetXResp",     SST::MemHierarchy::Command::GetXResp},
    {"FlushLineResp",SST::MemHierarchy::Command::FlushLineResp},
    {"FlushAllResp", SST::MemHierarchy::Command::FlushAllResp},
    {"PutS",         SST::MemHierarchy::Command::PutS},
    {"PutX",         SST::MemHierarchy::Command::PutX},
    {"PutE",         SST::MemHierarchy::Command::PutE},
    {"PutM",         SST::MemHierarchy::Command::PutM},
    {"FetchInv",     SST::MemHierarchy::Command::FetchInv}
};

} // anonymous namespace

/********** FaultInjectorMemH **********/

FaultInjectorMemH::FaultInjectorMemH(SST::Params& params) : PortModule() {
    pmId_ = params.find<std::string>("pmId", "");
    debugManagerLogic_ = params.find<bool>("debugManagerLogic", false);
    std::string regIdsStr = params.find<std::string>("pmRegistryIds", "default");
    pmRegistryIds_ = splitComma(regIdsStr);
    /* Defer registry resolution to init() or first use - caches/PortModules may be created before Hali/FaultInjManager. */

    // Parse install direction
    std::string install_dir = params.find<std::string>("installDirection", "Receive");
    if (install_dir == "Send") {
        installDirection_ = installDirection::Send;
    } else {
        installDirection_ = installDirection::Receive;
    }

    // Parse command to monitor
    std::string install_cmd = params.find<std::string>("Command", "PutM");
    auto cmdIt = commandMap.find(install_cmd);
    if (cmdIt != commandMap.end()) {
        cmd = cmdIt->second;
    } else {
        getSimulationOutput().output("Warning: Could not match '%s' to a MemHierarchy::Command, defaulting to Write\n",
                                     install_cmd.c_str());
        cmd = MemHierarchy::Command::Write;
    }

    // Initialize random number generator
    generator = std::default_random_engine(rd());

    // Parse injection probability
    injectionProbability_ = params.find<double>("injectionProbability", 0.5);
    if (injectionProbability_ < 0.0 || injectionProbability_ > 1.0) {
        getSimulationOutput().fatal(CALL_INFO_LONG, -1,
            "Injection probability must be in range [0.0, 1.0], got %f\n", injectionProbability_);
    }

    // Parse and configure fault type
    std::string faultType = params.find<std::string>("faultType", "");
    if (faultType == "stuckAt") {
        stuckAtInit(params);
        faultLogic = &FaultInjectorMemH::stuckAtFault;
    } else if (faultType == "randomFlip") {
        faultLogic = &FaultInjectorMemH::randomFlipFault;
    } else if (faultType == "randomDrop") {
        faultLogic = &FaultInjectorMemH::randomDropFault;
    } else if (faultType == "corruptMemRegion") {
        faultLogic = &FaultInjectorMemH::corruptMemRegionFault;
    } else if (faultType == "custom") {
        faultLogic = &FaultInjectorMemH::customFault;
    } else {
        getSimulationOutput().fatal(CALL_INFO_LONG, -1,
            "Invalid fault type '%s'. Valid options: stuckAt, randomFlip, randomDrop, corruptMemRegion, custom\n",
            faultType.c_str());
    }

#ifdef __SST_DEBUG_OUTPUT__
    getSimulationOutput().output("FaultInjectorMemH initialized: direction=%s, cmd=%s, probability=%f, faultType=%s\n",
                                 install_dir.c_str(), install_cmd.c_str(), injectionProbability_, faultType.c_str());
#endif
}

// Resolve pmRegistryIds_ to registry pointers (lazy, so Hali/FaultInjManager can exist first).
// Fatal if no registry found; then push RegisterPM(pmId_) once per registry if not yet sent.
void FaultInjectorMemH::ensureRegistriesResolved() {
    if (!registries_.empty()) return;
    for (const std::string& id : pmRegistryIds_) {
        PMDataRegistry* reg = PMRegistryResolver::getRegistry(id);
        if (reg) registries_.push_back(reg);
    }
    if (registries_.empty()) {
        std::string idsStr;
        for (size_t i = 0; i < pmRegistryIds_.size(); ++i) {
            if (i) idsStr += ",";
            idsStr += pmRegistryIds_[i];
        }
        getSimulationOutput().fatal(CALL_INFO_LONG, -1,
            "FaultInjectorMemH: no valid registry found for pmRegistryIds '%s'\n", idsStr.c_str());
    }
    if (!registerPMSent_) {
        for (PMDataRegistry* reg : registries_) {
            reg->pushMessageToManager(ManagerMessage::makeRegisterPM(pmId_));
        }
        registerPMSent_ = true;
        if (debugManagerLogic_) {
            std::string idsStr;
            for (size_t i = 0; i < pmRegistryIds_.size(); ++i) {
                if (i) idsStr += ",";
                idsStr += pmRegistryIds_[i];
            }
            getSimulationOutput().output("[ManagerLogic] FaultInjectorMemH pmId=%s: resolved registries [%s], sent RegisterPM to %zu manager(s)\n",
                pmId_.c_str(), idsStr.c_str(), registries_.size());
        }
    }
}

// At init phase 0, resolve registries and register this PM so we're ready before events flow.
void FaultInjectorMemH::init(unsigned phase) {
    if (phase == 0) {
        ensureRegistriesResolved();
    }
}

// Called when an event is sent. Ensures registries resolved (in case init didn't run first).
// If event carries PM data (e.g. injection_rate), skip fault injection; else apply fault if cmd matches.
void FaultInjectorMemH::eventSent(uintptr_t key, Event*& ev) {
    MemHierarchy::MemEvent* event = static_cast<MemHierarchy::MemEvent*>(ev);
    if (!event) {
        return;
    }

    ensureRegistriesResolved();
    auto eventId = event->getID();
    for (PMDataRegistry* reg : registries_) {
        if (reg->hasPMData(eventId)) {
            PMData pmData = reg->lookupPMData(eventId);
            if (debugManagerLogic_) {
                getSimulationOutput().output("[FaultInjectorMemH] pmId=%s read PM data: eventId=<%llu,%d> cmd=%s\n",
                    pmId_.c_str(), (unsigned long long)eventId.first, eventId.second, pmData.command.c_str());
            }
#ifdef __SST_DEBUG_OUTPUT__
            if (pmData.command == "injection_rate") {
                double rate = pmData.getParam<double>(0);
                getSimulationOutput().output("FaultInjectorMemH::eventSent: Event ID <%llu,%d> PM_CMD: %s (rate=%f)\n",
                                             eventId.first, eventId.second, pmData.command.c_str(), rate);
            } else {
                getSimulationOutput().output("FaultInjectorMemH::eventSent: Event ID <%llu,%d> PM_CMD: %s (params=%zu)\n",
                                             eventId.first, eventId.second, pmData.command.c_str(), pmData.paramCount());
            }
#endif
            return;  // Skip fault injection for PM events
        }
    }

    // Apply fault logic if command matches
    if (cmd == event->getCmd()) {
        (this->*(faultLogic))(ev);
    }
}

// Called when an event is received (installDirection Receive). Ensures registries resolved.
// If event carries PM data, skip fault injection; else apply fault logic to the event.
void FaultInjectorMemH::interceptHandler(uintptr_t key, Event*& ev, bool& cancel) {
    cancel = false;

    MemHierarchy::MemEventBase* event = static_cast<MemHierarchy::MemEventBase*>(ev);
    if (!event) {
        return;
    }

    ensureRegistriesResolved();
    auto eventId = event->getID();
    for (PMDataRegistry* reg : registries_) {
        if (reg->hasPMData(eventId)) {
            PMData pmData = reg->lookupPMData(eventId);
            if (debugManagerLogic_) {
                getSimulationOutput().output("[FaultInjectorMemH] pmId=%s read PM data: eventId=<%llu,%d> cmd=%s\n",
                    pmId_.c_str(), (unsigned long long)eventId.first, eventId.second, pmData.command.c_str());
            }
            if (pmData.command == "injection_rate") {
                double rate = pmData.getParam<double>(0);
                getSimulationOutput().output("FaultInjectorMemH::interceptHandler: Event ID <%llu,%d> PM_CMD: %s (rate=%f)\n",
                                             eventId.first, eventId.second, pmData.command.c_str(), rate);
            } else {
                getSimulationOutput().output("FaultInjectorMemH::interceptHandler: Event ID <%llu,%d> PM_CMD: %s (params=%zu)\n",
                                             eventId.first, eventId.second, pmData.command.c_str(), pmData.paramCount());
            }
            return;  // Skip fault injection for PM events
        }
    }

    // Apply fault logic
#ifdef __SST_DEBUG_OUTPUT__
    std::string before = event->getVerboseString(1);
    getSimulationOutput().output("Before fault: %s\n", before.c_str());
#endif

    (this->*(faultLogic))(ev);

#ifdef __SST_DEBUG_OUTPUT__
    std::string after = event->getVerboseString(1);
    getSimulationOutput().output("After fault: %s\n", after.c_str());
#endif
}

void FaultInjectorMemH::stuckAtFault(Event*& ev) {
    // TODO: Implement stuck-at fault logic
    // Read event payload and:
    //  - If stuckAtMap.at(addr) exists, compare all listed bits with payload value
    //  - If payload value does not match mapped value, add bit to flip mask
    //  - Once all stored bit values have been compared, use flip mask to modify address data
}

void FaultInjectorMemH::stuckAtInit(SST::Params& params) {
    // TODO: Implement initialization of stuckAtMap from Python parameters
}

void FaultInjectorMemH::randomFlipFault(Event*& ev) {
    MemHierarchy::MemEvent* event = static_cast<MemHierarchy::MemEvent*>(ev);
    if (!event) {
        return;
    }

    // Check if we should inject a fault based on probability
    bool shouldFlip = dist_fp(generator) <= injectionProbability_;
    if (!shouldFlip) {
        return;
    }

    // Get payload data
    size_t data_sz = event->getPayloadSize() / 8;
    if (data_sz == 0) {
        return;
    }

    MemHierarchy::MemEvent::dataVec payload = event->getPayload();

    // Choose random bit to flip
    std::uniform_int_distribution<uint64_t> dist_bit(0, data_sz * 8 - 1);
    uint64_t bit = dist_bit(generator);
    uint64_t byte_index = bit / 8;
    uint64_t bit_offset = bit % 8;

    // Create flip mask
    unsigned char mask = static_cast<unsigned char>(1 << bit_offset);

#ifdef __SST_DEBUG_OUTPUT__
    getSimulationOutput().output("RandomFlip: flipping bit %llu (byte %llu, offset %llu)\n",
                                 bit, byte_index, bit_offset);
    getSimulationOutput().output("  Before: 0x%02x\n", static_cast<int>(payload[byte_index]));
#endif

    // Apply the bit flip
    std::vector<unsigned char> result(data_sz);
    for (size_t i = 0; i < data_sz; ++i) {
        result[i] = (i == byte_index) ? (payload[i] ^ mask) : payload[i];
    }

#ifdef __SST_DEBUG_OUTPUT__
    getSimulationOutput().output("  After:  0x%02x\n", static_cast<int>(result[byte_index]));
#endif

    event->setPayload(result);
}

void FaultInjectorMemH::randomDropFault(Event*& ev) {
    // TODO: Implement random drop fault logic
}

void FaultInjectorMemH::corruptMemRegionFault(Event*& ev) {
    // TODO: Implement memory region corruption logic
}

void FaultInjectorMemH::customFault(Event*& ev) {
    // TODO: Implement custom fault logic (user-defined)
}
