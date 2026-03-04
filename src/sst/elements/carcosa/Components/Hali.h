// Copyright 2009-2024 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2024, NTESS
// All rights reserved.
//
// Portions are copyright of other developers:
// See the file CONTRIBUTORS.TXT in the top level directory
// of the distribution for more information.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#ifndef CARCOSA_HALI_COMPONENT_H
#define CARCOSA_HALI_COMPONENT_H

/**
 * Hali Component - Interface layer for Carcosa architecture
 *
 * This component provides an interface layer between:
 * - Sensors and CPUs in the vehicle simulation
 * - Memory hierarchy components (via highlink/lowlink)
 * - Other Hali components (for ring-based communication)
 *
 * Key features:
 * - Transparent passthrough for MemHierarchy events
 * - Fault injection support via FaultInjManager
 * - Ring topology support for Hali-to-Hali communication
 *
 * Lifecycle phases:
 * 1) Construction - Configure links, read params
 * 2) Init - Forward MemHierarchy init events, discover ring neighbors
 * 3) Setup - Initialize simulation state
 * 4) Run - Handle events from all connected components
 * 5) Complete - Forward cleanup events
 * 6) Finish - Report statistics
 * 7) Destruction - Cleanup
 */

#include <cstdint>
#include <sst/core/component.h>
#include <sst/core/link.h>
#include "sst/elements/carcosa/Components/CarcosaMemCtrl.h"
#include "sst/elements/carcosa/Components/FaultInjManagerAPI.h"
#include "sst/elements/carcosa/Components/InterceptionAgentAPI.h"
#include <utility>
#include <vector>

namespace SST {
namespace MemHierarchy {
class MemEvent;
}

namespace Carcosa {

class Hali : public SST::Component {
public:
    // SST ELI Registration
    SST_ELI_REGISTER_COMPONENT(
        Hali,
        "Carcosa",
        "Hali",
        SST_ELI_ELEMENT_VERSION(1,0,0),
        "Interface layer for sensor data in vehicle simulations",
        COMPONENT_CATEGORY_PROCESSOR
    )

    SST_ELI_DOCUMENT_PARAMS(
        {"Sensors", "Number of SensorComponents this interface receives from.", NULL},
        {"CPUs", "Number of Compute components the Hali sends to.", NULL},
        {"verbose", "Enable verbose output for debugging.", "false"},
        {"intercept_ranges", "Semicolon-separated base,size pairs (e.g. '0xBEEF0000,4096') for addresses to hand to InterceptionAgent.", ""}
    )

    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        {"interceptionAgent", "Optional agent for intercepted memory accesses (e.g. Carcosa.PingPongAgent). If unset, no interception.", "SST::Carcosa::InterceptionAgentAPI"}
    )

    SST_ELI_DOCUMENT_PORTS(
        {"sensor", "Link to SensorComponent", {"Carcosa.SensorEvent"}},
        {"left", "Link to left Hali in ring", {"Carcosa.HaliEvent"}},
        {"right", "Link to right Hali in ring", {"Carcosa.HaliEvent"}},
        {"cpu", "Link to compute Component", {"Carcosa.CpuEvent", "Carcosa.FaultInjEvent"}},
        {"memCtrl", "Link to memory controller", {"Carcosa.CpuEvent"}},
        {"highlink", "Link to memoryHierarchy (CPU/standardInterface side)", {}},
        {"lowlink", "Link to memoryHierarchy (Cache side)", {}}
    )

    // Constructor/Destructor
    Hali(SST::ComponentId_t id, SST::Params& params);
    virtual ~Hali();

    // SST Lifecycle methods
    void init(unsigned phase) override;
    void setup() override;
    void complete(unsigned phase) override;
    void finish() override;
    void emergencyShutdown() override;
    void printStatus(Output& out) override;

    // Event handlers
    void handleHaliEvent(SST::Event* ev);
    void handleSensorEvent(SST::Event* ev);
    void handleCpuEvent(SST::Event* ev);
    void handleMemCtrlEvent(SST::Event* ev);
    void lowlinkMemEvent(SST::Event* ev);
    void highlinkMemEvent(SST::Event* ev);

    /** Returns true if addr falls within any registered intercept range. */
    bool isInterceptedAddress(uint64_t addr) const;

private:
    // Configuration
    unsigned eventsToSend_;
    bool verbose_;

    // State tracking
    unsigned eventsReceived_;
    unsigned eventsForwarded_;
    unsigned eventsSent_;
    unsigned cpuEventCount_;

    // Ring topology state
    std::set<std::string> neighbors_;
    std::set<std::string>::iterator iter_;
    std::string leftHaliMsg_;
    std::string rightHaliMsg_;
    std::string cpuMsg_;
    std::string sensorMsg_;

    // Output
    SST::Output* out_;

    // Links
    SST::Link* leftHaliLink_;
    SST::Link* rightHaliLink_;
    SST::Link* sensorLink_;
    SST::Link* cpuLink_;
    SST::Link* memCtrlLink_;
    SST::Link* highlink_;
    SST::Link* lowlink_;

    // Subcomponents
    FaultInjManagerAPI* faultInjManager_;
    InterceptionAgentAPI* interceptionAgent_;

    // Interception: address ranges passed to agent when present
    std::vector<std::pair<uint64_t, uint64_t>> interceptRanges_;
};

} // namespace Carcosa
} // namespace SST

#endif // CARCOSA_HALI_COMPONENT_H
