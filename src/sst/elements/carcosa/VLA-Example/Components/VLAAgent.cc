#include "sst_config.h"
#include "sst/elements/carcosa/VLA-Example/Components/VLAAgent.h"
#include "sst/elements/memHierarchy/memEvent.h"
#include "sst/elements/memHierarchy/memTypes.h"
#include <cstring>
#include <climits>

using namespace SST;
using namespace SST::MemHierarchy;
using namespace SST::Carcosa;

VLAAgent::VLAAgent(ComponentId_t id, Params& params)
    : InterceptionAgentAPI(id, params)
{
    out_ = new Output("", 1, 0, Output::STDOUT);
    VlaFsm::Config cfg;
    cfg.numViTLayers    = params.find<int>("num_vit_layers", 24);
    cfg.numLLMLayers    = params.find<int>("num_llm_layers", 32);
    cfg.maxCycles       = params.find<int>("max_cycles", 1);
    cfg.initialSeqLen   = params.find<int>("initial_seq_len", 228);
    cfg.maxSeqLen       = params.find<int>("max_seq_len", 64);
    cfg.numActionTokens = params.find<int>("num_action_tokens", 1);
    if (cfg.numActionTokens < 1) cfg.numActionTokens = 1;
    fsm_.setConfig(cfg);

    hyadesRole_ = params.find<int>("hyades_role", 0);
    verbose_ = params.find<bool>("verbose", false);
    unsigned int seed = params.find<uint32_t>("rng_seed", 12345u);
    rng_ = new RNG::MarsagliaRNG(11, seed);
}

VLAAgent::~VLAAgent()
{
    delete out_;
    delete rng_;
}

bool VLAAgent::handleInterceptedEvent(MemEvent* ev, Link* highlink)
{
    (void)highlink;
    uint64_t offset = ev->getAddr() - controlAddrBase_;

    if (offset == 0x0000 && ev->getCmd() == Command::GetS) {
        if (fsm_.exitAfterThisRead()) {
            fsm_.clearExitFlag();
            sendCommandResponse(ev, -1);
            return true;
        }
        int cmd = static_cast<int>(fsm_.state());
        sendCommandResponse(ev, cmd);
        return true;
    }
    if (offset == 0x0008 && ev->getCmd() == Command::GetS) {
        sendCommandResponse(ev, fsm_.currentSeqLen());
        return true;
    }
    if (offset == 0x0010 && ev->getCmd() == Command::GetS) {
        sendCommandResponse(ev, hyadesRole_);
        return true;
    }
    if (offset == 0x0004 && (ev->getCmd() == Command::Write || ev->getCmd() == Command::GetX)) {
        sendWriteAck(ev);
        advanceFSM();
        return true;
    }
    return warnAndDropUnknownIntercept(ev, controlAddrBase_);
}

void VLAAgent::agentSetup()
{
    fsm_.reset();
    fsm_.validatePeakSeqLen(out_, "VLAAgent");

    if (verbose_) {
        const auto& cfg = fsm_.config();
        out_->output("VLAAgent: setup num_vit_layers=%d num_llm_layers=%d max_cycles=%d initial_seq_len=%d max_seq_len=%d hyades_role=%d\n",
                    cfg.numViTLayers, cfg.numLLMLayers, cfg.maxCycles, cfg.initialSeqLen, cfg.maxSeqLen, hyadesRole_);
    }
}

void VLAAgent::setInterceptBase(uint64_t base)
{
    controlAddrBase_ = base;
}

void VLAAgent::setHighlink(Link* highlink)
{
    highlink_ = highlink;
}

void VLAAgent::advanceFSM()
{
    VLAState prev = fsm_.advance(out_, "VLAAgent");
    if (verbose_) {
        out_->output("VLAAgent: %d -> %d (vit=%d prefill=%d decode=%d seqLen=%d)\n",
                    static_cast<int>(prev), static_cast<int>(fsm_.state()),
                    fsm_.vitLayer(), fsm_.prefillLayer(), fsm_.decodeLayer(), fsm_.currentSeqLen());
    }
}

void VLAAgent::sendCommandResponse(MemEvent* request, int value)
{
    MemEvent* resp = request->makeResponse();
    std::vector<uint8_t> data(4);
    std::memcpy(data.data(), &value, sizeof(int));
    resp->setPayload(data);
    if (highlink_) highlink_->send(resp);
    delete request;
}

void VLAAgent::sendWriteAck(MemEvent* ev)
{
    MemEvent* resp = ev->makeResponse();
    if (highlink_) highlink_->send(resp);
    delete ev;
}
