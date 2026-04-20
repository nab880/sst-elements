#include "sst_config.h"
#include "sst/elements/carcosa/VLA-Example/Components/VLACpuAgent.h"
#include "sst/elements/carcosa/Components/HaliEvent.h"
#include "sst/elements/memHierarchy/memEvent.h"
#include "sst/elements/memHierarchy/memTypes.h"
#include <cinttypes>
#include <cstring>
#include <climits>

using namespace SST;
using namespace SST::MemHierarchy;
using namespace SST::Carcosa;

VLACpuAgent::VLACpuAgent(ComponentId_t id, Params& params)
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

    verbose_      = params.find<bool>("verbose", false);
    unsigned int seed = params.find<uint32_t>("rng_seed", 12345u);
    rng_ = new RNG::MarsagliaRNG(11, seed);
}

VLACpuAgent::~VLACpuAgent()
{
    printProfile();
    delete out_;
    delete rng_;
}

bool VLACpuAgent::handleInterceptedEvent(MemEvent* ev, Link* highlink)
{
    (void)highlink;
    uint64_t offset = ev->getAddr() - controlAddrBase_;

    if (offset == 0x0000 && ev->getCmd() == Command::GetS) {
        if (nextCommand_ >= -1 && nextCommand_ != INT_MIN) {
            activeKernelId_ = nextCommand_;
            kernelStartCycle_ = getCurrentSimCycle();
            sendCommandResponse(ev, nextCommand_);
            nextCommand_ = INT_MIN;
        } else {
            pendingCommandRead_ = ev;
        }
        return true;
    }

    if (offset == 0x0008 && ev->getCmd() == Command::GetS) {
        sendCommandResponse(ev, fsm_.currentSeqLen());
        return true;
    }

    if (offset == 0x0010 && ev->getCmd() == Command::GetS) {
        sendCommandResponse(ev, 0);
        return true;
    }

    if (offset == 0x0004 &&
        (ev->getCmd() == Command::Write || ev->getCmd() == Command::GetX)) {
        sendWriteAck(ev);
        recordKernelEnd();
        localDone_ = true;
        if (verbose_)
            out_->output("VLACpuAgent: local done for state %d\n",
                         static_cast<int>(fsm_.state()));
        checkBothDone();
        return true;
    }

    return warnAndDropUnknownIntercept(ev, controlAddrBase_);
}

void VLACpuAgent::handleRingEvent(HaliEvent* ev)
{
    if (ev->getStr() == "done") {
        partnerDone_ = true;
        if (verbose_)
            out_->output("VLACpuAgent: GPU partner done\n");
        checkBothDone();
    }
}

void VLACpuAgent::agentSetup()
{
    fsm_.reset();
    nextCommand_ = static_cast<int>(IDLE);

    fsm_.validatePeakSeqLen(out_, "VLACpuAgent");

    if (verbose_) {
        const auto& cfg = fsm_.config();
        out_->output("VLACpuAgent: setup vit=%d llm=%d max_cycles=%d init_seq=%d max_seq=%d\n",
                     cfg.numViTLayers, cfg.numLLMLayers, cfg.maxCycles, cfg.initialSeqLen, cfg.maxSeqLen);
    }

    if (pendingCommandRead_) {
        sendCommandResponse(pendingCommandRead_, nextCommand_);
        pendingCommandRead_ = nullptr;
        nextCommand_ = INT_MIN;
    }
}

void VLACpuAgent::setRingLink(Link* leftLink)  { leftHaliLink_ = leftLink; }
void VLACpuAgent::setInterceptBase(uint64_t b)  { controlAddrBase_ = b; }
void VLACpuAgent::setHighlink(Link* h)          { highlink_ = h; }

void VLACpuAgent::checkBothDone()
{
    if (!localDone_ || !partnerDone_) return;
    localDone_   = false;
    partnerDone_ = false;

    advanceFSM();

    if (fsm_.exitAfterThisRead()) {
        nextCommand_ = -1;
        if (leftHaliLink_)
            leftHaliLink_->send(new HaliEvent("exit", 0u));
    } else {
        nextCommand_ = static_cast<int>(fsm_.state());
        dispatchToGpu();
    }

    if (pendingCommandRead_) {
        activeKernelId_ = nextCommand_;
        kernelStartCycle_ = getCurrentSimCycle();
        sendCommandResponse(pendingCommandRead_, nextCommand_);
        pendingCommandRead_ = nullptr;
        nextCommand_ = INT_MIN;
    }
}

void VLACpuAgent::dispatchToGpu()
{
    if (!leftHaliLink_) return;
    leftHaliLink_->send(
        new HaliEvent("seqlen", static_cast<unsigned>(fsm_.currentSeqLen())));
    leftHaliLink_->send(
        new HaliEvent("cmd", static_cast<unsigned>(fsm_.state())));
}

void VLACpuAgent::advanceFSM()
{
    VLAState prev = fsm_.advance(out_, "VLACpuAgent");
    if (verbose_)
        out_->output("VLACpuAgent: %d -> %d (vit=%d prefill=%d decode=%d seq=%d)\n",
                     static_cast<int>(prev), static_cast<int>(fsm_.state()),
                     fsm_.vitLayer(), fsm_.prefillLayer(), fsm_.decodeLayer(), fsm_.currentSeqLen());
}

void VLACpuAgent::sendCommandResponse(MemEvent* request, int value)
{
    MemEvent* resp = request->makeResponse();
    std::vector<uint8_t> data(4);
    std::memcpy(data.data(), &value, sizeof(int));
    resp->setPayload(data);
    if (highlink_) highlink_->send(resp);
    delete request;
}

void VLACpuAgent::sendWriteAck(MemEvent* ev)
{
    MemEvent* resp = ev->makeResponse();
    if (highlink_) highlink_->send(resp);
    delete ev;
}

void VLACpuAgent::recordKernelEnd()
{
    if (activeKernelId_ < 0) return;
    uint64_t endCycle = getCurrentSimCycle();
    profile_.push_back({"cpu", activeKernelId_, vlaStateName(activeKernelId_),
                        kernelStartCycle_, endCycle});
    activeKernelId_ = -1;
}

void VLACpuAgent::printProfile()
{
    if (profile_.empty()) return;
    out_->output("\n=== VLA Per-Kernel Profile (CPU) ===\n");
    out_->output("core,kernel_id,kernel_name,start_ps,end_ps,delta_ps\n");
    for (auto& r : profile_) {
        out_->output("%s,%d,%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                     r.core.c_str(), r.kernelId, r.kernelName.c_str(),
                     r.startCycle, r.endCycle, r.endCycle - r.startCycle);
    }
    out_->output("=== End CPU Profile (%zu records) ===\n\n",
                 profile_.size());
}
