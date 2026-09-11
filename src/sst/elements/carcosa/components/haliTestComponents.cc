#include "sst_config.h"
#include "sst/elements/carcosa/components/haliTestComponents.h"

using namespace SST;
using namespace SST::Carcosa;
using namespace SST::MemHierarchy;

HaliTestAgent::HaliTestAgent(ComponentId_t id, Params& params)
    : InterceptionAgentAPI(id, params) {
    out_ = new Output("", 1, 0, Output::STDOUT);
    mode_ = params.find<std::string>("mode", "payloadless_getx");
    if (mode_ == "deferred_complete") {
        self_ = configureSelfLink("complete", "1ns",
            new Event::Handler<HaliTestAgent, &HaliTestAgent::complete>(this));
    }
}

HaliTestAgent::~HaliTestAgent() { delete out_; }

bool HaliTestAgent::handleInterceptedEvent(MemEvent*, Link*) {
    out_->fatal(CALL_INFO, -1, "HaliTestAgent: payload-less GetX reached legacy API.\n");
    return true;
}

ControlResult HaliTestAgent::handleControlAccess(ControlAccess& acc) {
    if (mode_ == "posted_write") {
        if (acc.isWrite) {
            if (!acc.posted || acc.value != 0x12345678u)
                out_->fatal(CALL_INFO, -1, "HaliTestAgent: posted write decoded incorrectly.\n");
            posted_write_seen_ = true;
        } else {
            if (!posted_write_seen_)
                out_->fatal(CALL_INFO, -1, "HaliTestAgent: read overtook posted write.\n");
            acc.readValue = 0x12345678u;
        }
        return ControlResult::Handled;
    }
    if (mode_ == "payloadless_getx")
        out_->fatal(CALL_INFO, -1, "HaliTestAgent: payload-less GetX reached neutral API.\n");
    if (mode_ == "payload_getx") {
        if (!acc.isWrite || acc.value != 0x12345678u)
            out_->fatal(CALL_INFO, -1, "HaliTestAgent: payload-bearing GetX decoded incorrectly.\n");
        return ControlResult::Handled;
    }
    if (mode_ == "deferred_complete") {
        if (acc.isWrite) return ControlResult::Ignored;
        self_->send(new Event());
        return ControlResult::Deferred;
    }
    return acc.isWrite ? ControlResult::Ignored : ControlResult::Deferred;
}

void HaliTestAgent::complete(Event* ev) {
    delete ev;
    if (!channel_) out_->fatal(CALL_INFO, -1, "HaliTestAgent: missing ControlChannel.\n");
    channel_->completePendingRead(0xAABBCCDDu);
}

HaliTestDriver::HaliTestDriver(ComponentId_t id, Params& params) : Component(id) {
    out_ = new Output("", 1, 0, Output::STDOUT);
    base_ = params.find<uint64_t>("base", 0xBEEF0000);
    mode_ = params.find<std::string>("mode", "payloadless_getx");
    defer_ = mode_ == "double_defer";
    cpu_ = configureLink("cpu_side", new Event::Handler<HaliTestDriver, &HaliTestDriver::cpuEvent>(this));
    mem_ = configureLink("mem_side", new Event::Handler<HaliTestDriver, &HaliTestDriver::memEvent>(this));
    registerClock("1GHz", new Clock::Handler<HaliTestDriver, &HaliTestDriver::tick>(this));
    registerAsPrimaryComponent(); primaryComponentDoNotEndSim();
}

bool HaliTestDriver::tick(Cycle_t) {
    if (sent_) return false;
    sent_ = true;
    if (defer_) {
        cpu_->send(new MemEvent(getName(), base_, base_ & ~63ull, Command::GetS, 4));
        cpu_->send(new MemEvent(getName(), base_ + 4, base_ & ~63ull, Command::GetS, 4));
    } else if (mode_ == "deferred_complete") {
        cpu_->send(new MemEvent(getName(), base_, base_ & ~63ull, Command::GetS, 4));
    } else if (mode_ == "posted_write") {
        std::vector<uint8_t> payload = {0x78, 0x56, 0x34, 0x12};
        auto* write = new MemEvent(getName(), base_ + 4, base_ & ~63ull,
                                   Command::Write, payload);
        write->setFlag(MemEventBase::F_NORESPONSE);
        cpu_->send(write);
        cpu_->send(new MemEvent(getName(), base_, base_ & ~63ull, Command::GetS, 4));
    } else if (mode_ == "payload_getx") {
        std::vector<uint8_t> payload = {0x78, 0x56, 0x34, 0x12};
        cpu_->send(new MemEvent(getName(), base_, base_ & ~63ull,
                               Command::GetX, payload));
    } else {
        MemEvent* req = new MemEvent(getName(), base_, base_ & ~63ull,
                                     Command::GetX, 64);
        req->getPayload().clear();
        cpu_->send(req);
    }
    return false;
}

void HaliTestDriver::memEvent(Event* ev) {
    auto* req = dynamic_cast<MemEvent*>(ev);
    downstream_seen_ = true;
    if (!req || mode_ != "payloadless_getx" ||
        req->getCmd() != Command::GetX || req->getPayloadSize() != 0)
        out_->fatal(CALL_INFO, -1, "HaliTestDriver: forwarded request was not payload-less GetX.\n");
    MemEvent* resp = req->makeResponse();
    std::vector<uint8_t> payload(64, 0);
    resp->setPayload(payload);
    mem_->send(resp); delete req;
}

void HaliTestDriver::cpuEvent(Event* ev) {
    auto* resp = dynamic_cast<MemEvent*>(ev);
    if (!resp) out_->fatal(CALL_INFO, -1, "HaliTestDriver: non-MemEvent response.\n");
    if (mode_ == "posted_write") {
        if (resp->getCmd() != Command::GetSResp ||
            resp->getPayload() != std::vector<uint8_t>({0x78, 0x56, 0x34, 0x12}))
            out_->fatal(CALL_INFO, -1, "HaliTestDriver: posted write produced an acknowledgment or read returned wrong data.\n");
    }
    if (mode_ == "deferred_complete") {
        auto& p = resp->getPayload();
        if (p.size() < 4 || p[0] != 0xDD || p[1] != 0xCC ||
            p[2] != 0xBB || p[3] != 0xAA)
            out_->fatal(CALL_INFO, -1, "HaliTestDriver: deferred read returned wrong value.\n");
    }
    delete resp; passed_ = true; primaryComponentOKToEndSim();
}

void HaliTestDriver::finish() {
    if (!defer_ && !passed_)
        out_->fatal(CALL_INFO, -1, "HaliTestDriver: mode %s did not complete.\n", mode_.c_str());
    if ((mode_ == "payload_getx" || mode_ == "deferred_complete" || mode_ == "posted_write") && downstream_seen_)
        out_->fatal(CALL_INFO, -1, "HaliTestDriver: handled control access leaked downstream.\n");
    if (!defer_) out_->output("HaliTestDriver: PASS mode=%s.\n", mode_.c_str());
    delete out_; out_ = nullptr;
}
