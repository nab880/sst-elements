#include "sst_config.h"
#include "sst/elements/carcosa/VLA-Example/Components/VLAGpuDelayAgent.h"
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

VLAGpuDelayAgent::VLAGpuDelayAgent(ComponentId_t id, Params& params)
    : InterceptionAgentAPI(id, params)
{
    out_ = new Output("", 1, 0, Output::STDOUT);
    verbose_        = params.find<bool>("verbose", false);
    scaleFactor_    = params.find<double>("scale_factor", 1.0);
    scaleSeq_       = params.find<double>("scale_seq",    1.0);
    scaleDim_       = params.find<double>("scale_dim",    1.0);
    scaleVocab_     = params.find<double>("scale_vocab",  1.0);
    baselineSeqLen_ = params.find<int>("baseline_seq_len", 228);
    maxSeqLen_      = params.find<int>("max_seq_len",      64);

    bool anyPerDim = (scaleSeq_ != 1.0) || (scaleDim_ != 1.0) || (scaleVocab_ != 1.0);
    legacyScaling_ = !anyPerDim && (scaleFactor_ != 1.0);
    if (anyPerDim && scaleFactor_ != 1.0) {
        out_->output("VLAGpuDelayAgent: scale_factor=%.3f ignored because scale_seq/scale_dim/scale_vocab were set; using per-dimension scaling.\n",
                     scaleFactor_);
    }

    std::string csv = params.find<std::string>("baseline_ps", "");
    if (csv.empty()) {
        std::string legacy = params.find<std::string>("baseline_cycles", "");
        if (!legacy.empty()) {
            out_->output("VLAGpuDelayAgent: [deprecated] parameter 'baseline_cycles' is now 'baseline_ps'; values are interpreted as picoseconds.\n");
            csv = legacy;
        }
    }
    if (!csv.empty()) {
        parseBaselinePsCsv(csv, baselinePs_, out_, "VLAGpuDelayAgent");
    }

    selfLink_ = configureSelfLink("delay_self", "1ps",
        new Event::Handler<VLAGpuDelayAgent, &VLAGpuDelayAgent::handleDelayComplete>(this));
}

VLAGpuDelayAgent::~VLAGpuDelayAgent()
{
    printProfile();
    delete out_;
}

bool VLAGpuDelayAgent::handleInterceptedEvent(MemEvent* ev, Link* highlink)
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
        sendCommandResponse(ev, seqLen_);
        return true;
    }

    if (offset == 0x0010 && ev->getCmd() == Command::GetS) {
        sendCommandResponse(ev, 1);
        return true;
    }

    if (offset == 0x0004 &&
        (ev->getCmd() == Command::Write || ev->getCmd() == Command::GetX)) {
        sendWriteAck(ev);

        // Ignore status while self-link delay pending (avoid double done).
        if (delayPending_) {
            if (verbose_)
                out_->output("VLAGpuDelayAgent: dropping re-entrant status write (delay still pending)\n");
            return true;
        }

        uint64_t delayPs = computeScaledDelay(activeKernelId_);
        if (delayPs > 0) {
            delayPending_ = true;
            selfLink_->send(static_cast<SimTime_t>(delayPs), nullptr);
        } else {
            recordKernelEnd();
            if (verbose_)
                out_->output("VLAGpuDelayAgent: core done (0 delay), sending done\n");
            if (leftHaliLink_)
                leftHaliLink_->send(new HaliEvent("done", 0u));
        }
        return true;
    }

    return warnAndDropUnknownIntercept(ev, controlAddrBase_);
}

void VLAGpuDelayAgent::handleDelayComplete(Event* ev)
{
    delete ev;
    delayPending_ = false;
    recordKernelEnd();
    if (verbose_)
        out_->output("VLAGpuDelayAgent: core done (after delay), sending done\n");
    if (leftHaliLink_)
        leftHaliLink_->send(new HaliEvent("done", 0u));
}

void VLAGpuDelayAgent::handleRingEvent(HaliEvent* ev)
{
    const std::string& tag = ev->getStr();

    if (tag == "cmd") {
        nextCommand_ = static_cast<int>(ev->getNum());
        if (verbose_)
            out_->output("VLAGpuDelayAgent: received cmd %d\n", nextCommand_);
        if (pendingCommandRead_) {
            activeKernelId_ = nextCommand_;
            kernelStartCycle_ = getCurrentSimCycle();
            sendCommandResponse(pendingCommandRead_, nextCommand_);
            pendingCommandRead_ = nullptr;
            nextCommand_ = INT_MIN;
        }
    } else if (tag == "seqlen") {
        int incoming = static_cast<int>(ev->getNum());
        if (incoming > maxSeqLen_) {
            out_->fatal(CALL_INFO, -1,
                "VLAGpuDelayAgent: received seqlen=%d from CPU delay agent but max_seq_len=%d; "
                "binary KV-cache would overflow. Raise max_seq_len or lower CPU initial_seq_len/num_action_tokens.\n",
                incoming, maxSeqLen_);
        }
        seqLen_ = incoming;
        if (verbose_)
            out_->output("VLAGpuDelayAgent: received seqlen %d\n", seqLen_);
    } else if (tag == "exit") {
        nextCommand_ = -1;
        if (verbose_)
            out_->output("VLAGpuDelayAgent: received exit\n");
        if (pendingCommandRead_) {
            activeKernelId_ = -1;
            sendCommandResponse(pendingCommandRead_, -1);
            pendingCommandRead_ = nullptr;
            nextCommand_ = INT_MIN;
        }
    }
}

void VLAGpuDelayAgent::agentSetup()
{
    nextCommand_ = 0;
    seqLen_ = 0;

    if (verbose_) {
        if (legacyScaling_) {
            out_->output("VLAGpuDelayAgent: setup (initial cmd = IDLE) scale_factor=%.2f (legacy)\n",
                         scaleFactor_);
        } else {
            out_->output("VLAGpuDelayAgent: setup (initial cmd = IDLE) scale_seq=%.2f scale_dim=%.2f scale_vocab=%.2f baseline_seq_len=%d\n",
                         scaleSeq_, scaleDim_, scaleVocab_, baselineSeqLen_);
        }
    }

    if (pendingCommandRead_) {
        activeKernelId_ = nextCommand_;
        kernelStartCycle_ = getCurrentSimCycle();
        sendCommandResponse(pendingCommandRead_, nextCommand_);
        pendingCommandRead_ = nullptr;
        nextCommand_ = INT_MIN;
    }
}

void VLAGpuDelayAgent::setRingLink(Link* leftLink)  { leftHaliLink_ = leftLink; }
void VLAGpuDelayAgent::setInterceptBase(uint64_t b)  { controlAddrBase_ = b; }
void VLAGpuDelayAgent::setHighlink(Link* h)          { highlink_ = h; }

uint64_t VLAGpuDelayAgent::computeScaledDelay(int kernelId)
{
    if (kernelId < 0 || kernelId >= NUM_STATES) return 0;
    uint64_t base = baselinePs_[kernelId];
    if (base == 0) return 0;

    if (legacyScaling_)
        return computeLegacyScaledDelayPs(base, kernelId, scaleFactor_);

    // GPU has no FSM; currentSeqLen arrives over the ring; layer/token counters are 0.
    return computeScaledDelayPs(base, kernelId,
                                scaleSeq_, scaleDim_, scaleVocab_,
                                seqLen_,
                                baselineSeqLen_,
                                /*actionTokenCount*/0,
                                /*currentLayer*/0);
}

void VLAGpuDelayAgent::sendCommandResponse(MemEvent* request, int value)
{
    MemEvent* resp = request->makeResponse();
    std::vector<uint8_t> data(4);
    std::memcpy(data.data(), &value, sizeof(int));
    resp->setPayload(data);
    if (highlink_) highlink_->send(resp);
    delete request;
}

void VLAGpuDelayAgent::sendWriteAck(MemEvent* ev)
{
    MemEvent* resp = ev->makeResponse();
    if (highlink_) highlink_->send(resp);
    delete ev;
}

void VLAGpuDelayAgent::recordKernelEnd()
{
    if (activeKernelId_ < 0) return;
    uint64_t endCycle = getCurrentSimCycle();
    profile_.push_back({"gpu_delay", activeKernelId_, vlaStateName(activeKernelId_),
                        kernelStartCycle_, endCycle});
    activeKernelId_ = -1;
}

void VLAGpuDelayAgent::printProfile()
{
    if (profile_.empty()) return;
    out_->output("\n=== VLA Per-Kernel Profile (GPU Delay Agent) ===\n");
    out_->output("core,kernel_id,kernel_name,start_ps,end_ps,delta_ps\n");
    for (auto& r : profile_) {
        out_->output("%s,%d,%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                     r.core.c_str(), r.kernelId, r.kernelName.c_str(),
                     r.startCycle, r.endCycle, r.endCycle - r.startCycle);
    }
    out_->output("=== End GPU Delay Profile (%zu records) ===\n\n",
                 profile_.size());
}
