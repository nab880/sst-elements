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
    numViTLayers_ = params.find<int>("num_vit_layers", 24);
    numLLMLayers_ = params.find<int>("num_llm_layers", 32);
    maxCycles_ = params.find<int>("max_cycles", 1);
    initialSeqLen_ = params.find<int>("initial_seq_len", 228);
    maxSeqLen_ = params.find<int>("max_seq_len", 64);
    numActionTokens_ = params.find<int>("num_action_tokens", 1);
    if (numActionTokens_ < 1) numActionTokens_ = 1;
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
        if (exitAfterThisRead_) {
            exitAfterThisRead_ = false;
            sendCommandResponse(ev, -1);
            return true;
        }
        int cmd = static_cast<int>(currentState_);
        sendCommandResponse(ev, cmd);
        return true;
    }
    if (offset == 0x0008 && ev->getCmd() == Command::GetS) {
        sendCommandResponse(ev, currentSeqLen_);
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
    currentState_ = IDLE;
    vitLayer_ = 0;
    prefillLayer_ = 0;
    decodeLayer_ = 0;
    actionTokenCount_ = 0;
    currentSeqLen_ = 0;

    int peakSeqLen = initialSeqLen_ + (numActionTokens_ - 1);
    if (peakSeqLen > maxSeqLen_) {
        out_->fatal(CALL_INFO, -1,
            "VLAAgent: peak sequence length %d (initial_seq_len=%d + num_action_tokens-1=%d) "
            "exceeds max_seq_len=%d. The binary's KV cache (MAX_SEQ_LEN) would overflow. "
            "Lower initial_seq_len/num_action_tokens or rebuild the binary with a matching MAX_SEQ_LEN.\n",
            peakSeqLen, initialSeqLen_, numActionTokens_ - 1, maxSeqLen_);
    }

    if (verbose_) {
        out_->output("VLAAgent: setup num_vit_layers=%d num_llm_layers=%d max_cycles=%d initial_seq_len=%d max_seq_len=%d hyades_role=%d\n",
                    numViTLayers_, numLLMLayers_, maxCycles_, initialSeqLen_, maxSeqLen_, hyadesRole_);
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
    VLAState next = currentState_;

    switch (currentState_) {
    case IDLE:
        next = VISION_INGESTION;
        break;
    case VISION_INGESTION:
        next = PATCHIFICATION_EMBED;
        break;
    case PATCHIFICATION_EMBED:
        next = VIS_ATTN_PROJ;
        break;
    case VIS_ATTN_PROJ:
        next = GLOBAL_SPATIAL_ATTN;
        break;
    case GLOBAL_SPATIAL_ATTN:
        next = VIS_FFN;
        break;
    case VIS_FFN:
        vitLayer_++;
        if (vitLayer_ < numViTLayers_)
            next = VIS_ATTN_PROJ;
        else
            next = MLP_PROJECTOR;
        break;
    case MLP_PROJECTOR:
        next = SEQ_CONCAT;
        break;
    case SEQ_CONCAT:
        next = PREFILL_ATTN_PROJ;
        currentSeqLen_ = initialSeqLen_;
        break;
    case PREFILL_ATTN_PROJ:
        next = PREFILL_CAUSAL_ATTN;
        break;
    case PREFILL_CAUSAL_ATTN:
        next = PREFILL_FFN;
        break;
    case PREFILL_FFN:
        prefillLayer_++;
        if (prefillLayer_ < numLLMLayers_)
            next = PREFILL_ATTN_PROJ;
        else
            next = GEMV_PROJECT;
        break;
    case GEMV_PROJECT:
        next = KV_CACHE_ATTN;
        break;
    case KV_CACHE_ATTN:
        next = DECODE_FFN;
        break;
    case DECODE_FFN:
        decodeLayer_++;
        if (decodeLayer_ < numLLMLayers_)
            next = GEMV_PROJECT;
        else {
            next = LM_HEAD;
            decodeLayer_ = 0;
        }
        break;
    case LM_HEAD: {
        actionTokenCount_++;
        if (actionTokenCount_ < numActionTokens_) {
            if (currentSeqLen_ + 1 > maxSeqLen_) {
                out_->fatal(CALL_INFO, -1,
                    "VLAAgent: currentSeqLen_ would become %d and exceed max_seq_len=%d; "
                    "binary KV-cache overflow would occur. Check initial_seq_len/num_action_tokens vs binary MAX_SEQ_LEN.\n",
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
    case DETOK_DEQUANT:
        next = FAST_IDCT;
        break;
    case FAST_IDCT:
        next = ACTUATE;
        break;
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
    default:
        break;
    }

    if (verbose_) {
        out_->output("VLAAgent: %d -> %d (vit=%d prefill=%d decode=%d seqLen=%d)\n",
                    static_cast<int>(currentState_), static_cast<int>(next),
                    vitLayer_, prefillLayer_, decodeLayer_, currentSeqLen_);
    }
    currentState_ = next;
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
