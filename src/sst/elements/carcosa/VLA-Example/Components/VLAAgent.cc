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
    cfg.numViTLayers         = params.find<int>("num_vit_layers", 24);
    cfg.numLLMLayers         = params.find<int>("num_llm_layers", 32);
    cfg.maxCycles            = params.find<int>("max_cycles", 1);
    cfg.initialSeqLen        = params.find<int>("initial_seq_len", 228);
    cfg.maxSeqLen            = params.find<int>("max_seq_len", 64);
    cfg.numActionTokens      = params.find<int>("num_action_tokens", 1);
    if (cfg.numActionTokens < 1) cfg.numActionTokens = 1;
    cfg.decodeEarlyExitProb  = params.find<double>("decode_exit_prob", 0.0);
    cfg.rngSeed              = params.find<uint32_t>("rng_seed", 12345u);
    fsm_.setConfig(cfg);

    if (cfg.decodeEarlyExitProb < 0.0 || cfg.decodeEarlyExitProb > 1.0) {
        out_->fatal(CALL_INFO, -1,
            "VLAAgent: decode_exit_prob=%.6f is out of range [0.0, 1.0].\n",
            cfg.decodeEarlyExitProb);
    }

    hyadesRole_ = params.find<int>("hyades_role", 0);
    verbose_ = params.find<bool>("verbose", false);

    stateKey_   = params.find<std::string>("state_key", "");
    regionSize_ = params.find<uint64_t>("region_size", 4096);
}

VLAAgent::~VLAAgent()
{
    delete out_;
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
        publishKernel(KERNEL_IDLE);
        advanceFSM();
        return true;
    }
    return warnAndDropUnknownIntercept(ev, controlAddrBase_);
}

void VLAAgent::agentSetup()
{
    fsm_.reset();
    fsm_.validatePeakSeqLen(out_, "VLAAgent");

    if (!stateKey_.empty()) {
        PipelineStateBase* s =
            PipelineStateRegistry<PipelineStateBase>::getOrCreate(stateKey_);
        s->currentKernel = KERNEL_IDLE;
        s->pipelineCycle = 0;
        publishMmioRegion();
    }

    if (verbose_) {
        const auto& cfg = fsm_.config();
        out_->output("VLAAgent: setup num_vit_layers=%d num_llm_layers=%d max_cycles=%d initial_seq_len=%d max_seq_len=%d hyades_role=%d state_key=%s\n",
                    cfg.numViTLayers, cfg.numLLMLayers, cfg.maxCycles, cfg.initialSeqLen, cfg.maxSeqLen, hyadesRole_,
                    stateKey_.empty() ? "<unset>" : stateKey_.c_str());
    }
}

void VLAAgent::setInterceptBase(uint64_t base)
{
    controlAddrBase_ = base;
    if (!stateKey_.empty()) publishMmioRegion();
}

void VLAAgent::publishMmioRegion()
{
    PipelineStateBase* s =
        PipelineStateRegistry<PipelineStateBase>::getMutable(stateKey_);
    if (!s) return;
    s->ensureRegionSlot(0);
    s->regions[0].base  = controlAddrBase_;
    s->regions[0].size  = regionSize_;
    s->regions[0].valid = regionSize_ > 0;
    s->regions[0].id    = 0;
    s->regions[0].name  = "mmio_control";
}

void VLAAgent::publishKernel(int kernel)
{
    if (stateKey_.empty()) return;
    PipelineStateBase* s =
        PipelineStateRegistry<PipelineStateBase>::getMutable(stateKey_);
    if (!s) s = PipelineStateRegistry<PipelineStateBase>::getOrCreate(stateKey_);
    s->currentKernel = kernel;
    s->pipelineCycle = fsm_.pipelineCycles();
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
    // Publish before sending so any PortModule on the lowlink already sees the
    // updated currentKernel by the time the CPU's first handler memop arrives.
    publishKernel(value >= 0 ? value : KERNEL_IDLE);

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
