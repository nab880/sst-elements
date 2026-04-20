#include "sst_config.h"
#include "sst/elements/carcosa/VLA-Example/Components/VLACpuDelayAgent.h"
#include "sst/elements/carcosa/VLA-Example/Components/VLAKernelComplexity.h"
#include "sst/elements/carcosa/Components/HaliEvent.h"
#include "sst/elements/memHierarchy/memEvent.h"
#include "sst/elements/memHierarchy/memTypes.h"
#include <cinttypes>
#include <cstring>
#include <climits>
#include <sstream>

using namespace SST;
using namespace SST::MemHierarchy;
using namespace SST::Carcosa;

VLACpuDelayAgent::VLACpuDelayAgent(ComponentId_t id, Params& params)
    : InterceptionAgentAPI(id, params)
{
    out_ = new Output("", 1, 0, Output::STDOUT);
    VlaFsm::Config cfg;
    cfg.numViTLayers        = params.find<int>("num_vit_layers", 24);
    cfg.numLLMLayers        = params.find<int>("num_llm_layers", 32);
    cfg.maxCycles           = params.find<int>("max_cycles", 1);
    cfg.initialSeqLen       = params.find<int>("initial_seq_len", 228);
    cfg.maxSeqLen           = params.find<int>("max_seq_len", 64);
    cfg.numActionTokens     = params.find<int>("num_action_tokens", 1);
    if (cfg.numActionTokens < 1) cfg.numActionTokens = 1;
    cfg.decodeEarlyExitProb = params.find<double>("decode_exit_prob", 0.0);
    cfg.rngSeed             = params.find<uint32_t>("rng_seed", 12345u);
    fsm_.setConfig(cfg);

    if (cfg.decodeEarlyExitProb < 0.0 || cfg.decodeEarlyExitProb > 1.0) {
        out_->fatal(CALL_INFO, -1,
            "VLACpuDelayAgent: decode_exit_prob=%.6f is out of range [0.0, 1.0].\n",
            cfg.decodeEarlyExitProb);
    }

    verbose_       = params.find<bool>("verbose", false);
    scaleFactor_   = params.find<double>("scale_factor", 1.0);

    std::string csv = params.find<std::string>("baseline_ps", "");
    if (csv.empty()) {
        std::string legacy = params.find<std::string>("baseline_cycles", "");
        if (!legacy.empty()) {
            out_->output("VLACpuDelayAgent: [deprecated] parameter 'baseline_cycles' is now 'baseline_ps'; values are interpreted as picoseconds.\n");
            csv = legacy;
        }
    }
    if (!csv.empty()) {
        parseBaselinePsCsv(csv, baselinePs_, out_, "VLACpuDelayAgent");
    }

    selfLink_ = configureSelfLink("delay_self", "1ps",
        new Event::Handler<VLACpuDelayAgent, &VLACpuDelayAgent::handleDelayComplete>(this));
}

VLACpuDelayAgent::~VLACpuDelayAgent()
{
    printProfile();
    delete out_;
}

bool VLACpuDelayAgent::handleInterceptedEvent(MemEvent* ev, Link* highlink)
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

        // Ignore status while self-link delay pending (avoid double FSM advance).
        if (delayPending_) {
            if (verbose_)
                out_->output("VLACpuDelayAgent: dropping re-entrant status write (delay still pending, state %d)\n",
                             static_cast<int>(fsm_.state()));
            return true;
        }

        uint64_t delayPs = computeScaledDelay(activeKernelId_);
        if (delayPs > 0) {
            delayPending_ = true;
            selfLink_->send(static_cast<SimTime_t>(delayPs), nullptr);
        } else {
            recordKernelEnd();
            localDone_ = true;
            if (verbose_)
                out_->output("VLACpuDelayAgent: local done (0 delay) state %d\n",
                             static_cast<int>(fsm_.state()));
            checkBothDone();
        }
        return true;
    }

    return warnAndDropUnknownIntercept(ev, controlAddrBase_);
}

void VLACpuDelayAgent::handleDelayComplete(Event* ev)
{
    delete ev;
    delayPending_ = false;
    recordKernelEnd();
    localDone_ = true;
    if (verbose_)
        out_->output("VLACpuDelayAgent: local done (after delay) state %d\n",
                     static_cast<int>(fsm_.state()));
    checkBothDone();
}

void VLACpuDelayAgent::handleRingEvent(HaliEvent* ev)
{
    if (ev->getStr() == "done") {
        partnerDone_ = true;
        if (verbose_)
            out_->output("VLACpuDelayAgent: GPU partner done\n");
        checkBothDone();
    }
}

void VLACpuDelayAgent::agentSetup()
{
    fsm_.reset();
    nextCommand_ = static_cast<int>(IDLE);

    fsm_.validatePeakSeqLen(out_, "VLACpuDelayAgent");

    if (verbose_) {
        const auto& cfg = fsm_.config();
        out_->output("VLACpuDelayAgent: setup vit=%d llm=%d scale=%.2f max_seq=%d\n",
                     cfg.numViTLayers, cfg.numLLMLayers, scaleFactor_, cfg.maxSeqLen);
    }

    if (pendingCommandRead_) {
        activeKernelId_ = nextCommand_;
        kernelStartCycle_ = getCurrentSimCycle();
        sendCommandResponse(pendingCommandRead_, nextCommand_);
        pendingCommandRead_ = nullptr;
        nextCommand_ = INT_MIN;
    }
}

void VLACpuDelayAgent::setRingLink(Link* leftLink)  { leftHaliLink_ = leftLink; }
void VLACpuDelayAgent::setInterceptBase(uint64_t b)  { controlAddrBase_ = b; }
void VLACpuDelayAgent::setHighlink(Link* h)          { highlink_ = h; }

void VLACpuDelayAgent::checkBothDone()
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

void VLACpuDelayAgent::dispatchToGpu()
{
    if (!leftHaliLink_) return;
    leftHaliLink_->send(
        new HaliEvent("seqlen", static_cast<unsigned>(fsm_.currentSeqLen())));
    leftHaliLink_->send(
        new HaliEvent("cmd", static_cast<unsigned>(fsm_.state())));
}

void VLACpuDelayAgent::advanceFSM()
{
    VLAState prev = fsm_.advance(out_, "VLACpuDelayAgent");
    if (verbose_)
        out_->output("VLACpuDelayAgent: %d -> %d (vit=%d prefill=%d decode=%d seq=%d)\n",
                     static_cast<int>(prev), static_cast<int>(fsm_.state()),
                     fsm_.vitLayer(), fsm_.prefillLayer(), fsm_.decodeLayer(), fsm_.currentSeqLen());
}

uint64_t VLACpuDelayAgent::computeScaledDelay(int kernelId)
{
    if (kernelId < 0 || kernelId >= NUM_STATES) return 0;
    uint64_t base = baselinePs_[kernelId];
    if (base == 0) return 0;

    int order = complexityOrder(kernelId);
    double scaled = static_cast<double>(base);
    for (int i = 0; i < order; ++i)
        scaled *= scaleFactor_;

    return static_cast<uint64_t>(scaled);
}

void VLACpuDelayAgent::sendCommandResponse(MemEvent* request, int value)
{
    MemEvent* resp = request->makeResponse();
    std::vector<uint8_t> data(4);
    std::memcpy(data.data(), &value, sizeof(int));
    resp->setPayload(data);
    if (highlink_) highlink_->send(resp);
    delete request;
}

void VLACpuDelayAgent::sendWriteAck(MemEvent* ev)
{
    MemEvent* resp = ev->makeResponse();
    if (highlink_) highlink_->send(resp);
    delete ev;
}

void VLACpuDelayAgent::recordKernelEnd()
{
    if (activeKernelId_ < 0) return;
    uint64_t endCycle = getCurrentSimCycle();
    profile_.push_back({"cpu_delay", activeKernelId_, vlaStateName(activeKernelId_),
                        kernelStartCycle_, endCycle});
    activeKernelId_ = -1;
}

void VLACpuDelayAgent::printProfile()
{
    if (profile_.empty()) return;
    out_->output("\n=== VLA Per-Kernel Profile (CPU Delay Agent) ===\n");
    out_->output("core,kernel_id,kernel_name,start_ps,end_ps,delta_ps\n");
    for (auto& r : profile_) {
        out_->output("%s,%d,%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                     r.core.c_str(), r.kernelId, r.kernelName.c_str(),
                     r.startCycle, r.endCycle, r.endCycle - r.startCycle);
    }
    out_->output("=== End CPU Delay Profile (%zu records) ===\n\n",
                 profile_.size());
}
