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
    numViTLayers_ = params.find<int>("num_vit_layers", 24);
    numLLMLayers_ = params.find<int>("num_llm_layers", 32);
    maxCycles_    = params.find<int>("max_cycles", 1);
    initialSeqLen_ = params.find<int>("initial_seq_len", 228);
    maxSeqLen_    = params.find<int>("max_seq_len", 64);
    numActionTokens_ = params.find<int>("num_action_tokens", 1);
    if (numActionTokens_ < 1) numActionTokens_ = 1;
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
        sendCommandResponse(ev, currentSeqLen_);
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
                         static_cast<int>(currentState_));
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
    currentState_ = IDLE;
    vitLayer_ = 0;
    prefillLayer_ = 0;
    decodeLayer_ = 0;
    actionTokenCount_ = 0;
    currentSeqLen_ = 0;
    nextCommand_ = static_cast<int>(IDLE);

    int peakSeqLen = initialSeqLen_ + (numActionTokens_ - 1);
    if (peakSeqLen > maxSeqLen_) {
        out_->fatal(CALL_INFO, -1,
            "VLACpuAgent: peak sequence length %d (initial_seq_len=%d + num_action_tokens-1=%d) "
            "exceeds max_seq_len=%d. Binary KV-cache would overflow.\n",
            peakSeqLen, initialSeqLen_, numActionTokens_ - 1, maxSeqLen_);
    }

    if (verbose_)
        out_->output("VLACpuAgent: setup vit=%d llm=%d max_cycles=%d init_seq=%d max_seq=%d\n",
                     numViTLayers_, numLLMLayers_, maxCycles_, initialSeqLen_, maxSeqLen_);

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

    if (exitAfterThisRead_) {
        nextCommand_ = -1;
        if (leftHaliLink_)
            leftHaliLink_->send(new HaliEvent("exit", 0u));
    } else {
        nextCommand_ = static_cast<int>(currentState_);
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
        new HaliEvent("seqlen", static_cast<unsigned>(currentSeqLen_)));
    leftHaliLink_->send(
        new HaliEvent("cmd", static_cast<unsigned>(currentState_)));
}

void VLACpuAgent::advanceFSM()
{
    VLAState next = currentState_;

    switch (currentState_) {
    case IDLE:                next = VISION_INGESTION;     break;
    case VISION_INGESTION:    next = PATCHIFICATION_EMBED; break;
    case PATCHIFICATION_EMBED:next = VIS_ATTN_PROJ;        break;
    case VIS_ATTN_PROJ:       next = GLOBAL_SPATIAL_ATTN;  break;
    case GLOBAL_SPATIAL_ATTN: next = VIS_FFN;              break;
    case VIS_FFN:
        vitLayer_++;
        next = (vitLayer_ < numViTLayers_) ? VIS_ATTN_PROJ : MLP_PROJECTOR;
        break;
    case MLP_PROJECTOR:       next = SEQ_CONCAT;           break;
    case SEQ_CONCAT:
        next = PREFILL_ATTN_PROJ;
        currentSeqLen_ = initialSeqLen_;
        break;
    case PREFILL_ATTN_PROJ:   next = PREFILL_CAUSAL_ATTN;  break;
    case PREFILL_CAUSAL_ATTN: next = PREFILL_FFN;           break;
    case PREFILL_FFN:
        prefillLayer_++;
        next = (prefillLayer_ < numLLMLayers_) ? PREFILL_ATTN_PROJ : GEMV_PROJECT;
        break;
    case GEMV_PROJECT:        next = KV_CACHE_ATTN;        break;
    case KV_CACHE_ATTN:       next = DECODE_FFN;           break;
    case DECODE_FFN:
        decodeLayer_++;
        if (decodeLayer_ < numLLMLayers_) {
            next = GEMV_PROJECT;
        } else {
            next = LM_HEAD;
            decodeLayer_ = 0;
        }
        break;
    case LM_HEAD: {
        actionTokenCount_++;
        if (actionTokenCount_ < numActionTokens_) {
            if (currentSeqLen_ + 1 > maxSeqLen_) {
                out_->fatal(CALL_INFO, -1,
                    "VLACpuAgent: currentSeqLen_ would become %d and exceed max_seq_len=%d; "
                    "binary KV-cache overflow would occur.\n",
                    currentSeqLen_ + 1, maxSeqLen_);
            }
            currentSeqLen_++;
            decodeLayer_ = 0;
            next = GEMV_PROJECT;
        } else {
            actionTokenCount_ = 0;
            next = DETOK_DEQUANT;
        }
        break;
    }
    case DETOK_DEQUANT:       next = FAST_IDCT;            break;
    case FAST_IDCT:           next = ACTUATE;              break;
    case ACTUATE:
        next = IDLE;
        vitLayer_ = 0;
        prefillLayer_ = 0;
        decodeLayer_ = 0;
        actionTokenCount_ = 0;
        pipelineCycles_++;
        if (maxCycles_ > 0 && pipelineCycles_ >= maxCycles_)
            exitAfterThisRead_ = true;
        break;
    default: break;
    }

    if (verbose_)
        out_->output("VLACpuAgent: %d -> %d (vit=%d prefill=%d decode=%d seq=%d)\n",
                     static_cast<int>(currentState_), static_cast<int>(next),
                     vitLayer_, prefillLayer_, decodeLayer_, currentSeqLen_);
    currentState_ = next;
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
