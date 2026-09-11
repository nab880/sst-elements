#ifndef SST_ELEMENTS_CARCOSA_HALI_TEST_COMPONENTS_H
#define SST_ELEMENTS_CARCOSA_HALI_TEST_COMPONENTS_H

#include "sst/elements/carcosa/components/interceptionAgentAPI.h"
#include <sst/core/component.h>
#include <sst/core/output.h>
#include <string>
#include <vector>

namespace SST { namespace Carcosa {

class HaliTestAgent : public InterceptionAgentAPI {
public:
    SST_ELI_REGISTER_SUBCOMPONENT(HaliTestAgent, "carcosa", "HaliTestAgent",
        SST_ELI_ELEMENT_VERSION(1,0,0), "Self-asserting Hali control test agent.",
        SST::Carcosa::InterceptionAgentAPI)
    HaliTestAgent(ComponentId_t id, Params& params);
    HaliTestAgent() : InterceptionAgentAPI() {}
    ~HaliTestAgent() override;
    bool handleInterceptedEvent(SST::MemHierarchy::MemEvent*, SST::Link*) override;
    ControlResult handleControlAccess(ControlAccess&) override;
    void setControlChannel(ControlChannel* ch) override { channel_ = ch; }
private:
    void complete(Event* ev);
    SST::Output* out_ = nullptr;
    std::string mode_;
    ControlChannel* channel_ = nullptr;
    Link* self_ = nullptr;
    bool posted_write_seen_ = false;
};

class HaliTestDriver : public SST::Component {
public:
    SST_ELI_REGISTER_COMPONENT(HaliTestDriver, "carcosa", "HaliTestDriver",
        SST_ELI_ELEMENT_VERSION(1,0,0), "Self-asserting Hali data-plane driver.",
        COMPONENT_CATEGORY_UNCATEGORIZED)
    SST_ELI_DOCUMENT_PARAMS({"mode", "payloadless_getx or double_defer", "payloadless_getx"},
                            {"base", "Intercept range base.", "3203334144"})
    SST_ELI_DOCUMENT_PORTS(
        {"cpu_side", "Connect to Hali highlink.", {"memHierarchy.MemEventBase"}},
        {"mem_side", "Connect to Hali lowlink.", {"memHierarchy.MemEventBase"}})
    HaliTestDriver(ComponentId_t id, Params& params);
    void finish() override;
private:
    bool tick(Cycle_t);
    void cpuEvent(Event*);
    void memEvent(Event*);
    Output* out_ = nullptr;
    Link *cpu_ = nullptr, *mem_ = nullptr;
    uint64_t base_ = 0;
    std::string mode_;
    bool defer_ = false, sent_ = false, passed_ = false, downstream_seen_ = false;
};

}} // namespace SST::Carcosa
#endif
