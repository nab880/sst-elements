#include "sst_config.h"
#include "sst/elements/carcosa/VLA-Example/Components/VLACpuDelayAgent.h"
#include "sst/elements/carcosa/VLA-Example/Components/VLAKernelComplexity.h"
#include "sst/elements/carcosa/VLA-Example/Components/VlaRegions.h"
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

    verbose_        = params.find<bool>("verbose", false);
    scaleFactor_    = params.find<double>("scale_factor", 1.0);
    scaleSeq_       = params.find<double>("scale_seq",    1.0);
    scaleDim_       = params.find<double>("scale_dim",    1.0);
    scaleVocab_     = params.find<double>("scale_vocab",  1.0);
    baselineSeqLen_ = params.find<int>("baseline_seq_len", 0);
    if (baselineSeqLen_ <= 0) baselineSeqLen_ = cfg.initialSeqLen;

    stateKey_    = params.find<std::string>("state_key", "");
    regionSize_  = params.find<uint64_t>("region_size", 4096);
    regionsCsv_  = params.find<std::string>("regions", "");

    // Legacy single-factor path is used only when the user migrated no per-dim
    // scale and set scale_factor to something non-trivial. Defaults (everything
    // 1.0) go through the new path and produce factor=1.0 -> bit-identical.
    // Legacy path only when no per-dim scale was set but scale_factor != 1.0.
    bool anyPerDim = (scaleSeq_ != 1.0) || (scaleDim_ != 1.0) || (scaleVocab_ != 1.0);
    legacyScaling_ = !anyPerDim && (scaleFactor_ != 1.0);
    if (anyPerDim && scaleFactor_ != 1.0) {
        out_->output("VLACpuDelayAgent: scale_factor=%.3f ignored because scale_seq/scale_dim/scale_vocab were set; using per-dimension scaling.\n",
                     scaleFactor_);
    }

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

    // Workload region-publish ABI: write base_lo / base_hi / size / commit-slot
    // into staging and apply to PipelineStateBase::regions[slot] on commit.
    // The staged virtual base/size are what EccGuard matches against (via the
    // MemEvent's preserved virtual address), so the addresses the binary
    // actually touches now match the published regions.
    if ((offset == 0x0020 || offset == 0x0024 || offset == 0x0028 || offset == 0x002C) &&
        (ev->getCmd() == Command::Write || ev->getCmd() == Command::GetX)) {
        uint32_t value = 0;
        const auto& payload = ev->getPayload();
        if (payload.size() >= sizeof(uint32_t))
            std::memcpy(&value, payload.data(), sizeof(uint32_t));
        sendWriteAck(ev);
        applyRegionPublish(offset, value);
        return true;
    }

    // Per-frame action checksum publish (HYADES_ACTION_CHECKSUM_OFFSET).
    // Workload computes a fold-hash over action_queue (or any payload an
    // Escape would corrupt) and writes here; we stamp it onto the next
    // FrameRecord. See VLACpuDelayAgent::advanceFSM for the consumer.
    if (offset == 0x0030 &&
        (ev->getCmd() == Command::Write || ev->getCmd() == Command::GetX)) {
        uint32_t value = 0;
        const auto& payload = ev->getPayload();
        if (payload.size() >= sizeof(uint32_t))
            std::memcpy(&value, payload.data(), sizeof(uint32_t));
        sendWriteAck(ev);
        latestActionChecksum_    = value;
        latestActionChecksumSet_ = true;
        if (verbose_)
            out_->output("VLACpuDelayAgent: action checksum staged 0x%08x\n", value);
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
            // Re-affirm currentKernel for the full modeled delay window. Vanadis
            // is blocked on the next command read for `delayPs`, but L1/L2/dir
            // traffic from speculative loads, writebacks and the stub's region
            // walks must be attributed to this kernel until handleDelayComplete
            // publishes IDLE.
            publishKernel(activeKernelId_);
            selfLink_->send(static_cast<SimTime_t>(delayPs), nullptr);
        } else {
            recordKernelEnd();
            publishKernel(KERNEL_IDLE);
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
    publishKernel(KERNEL_IDLE);
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

    if (!stateKey_.empty()) {
        PipelineStateBase* s =
            PipelineStateRegistry<PipelineStateBase>::getOrCreate(stateKey_);
        s->currentKernel = KERNEL_IDLE;
        s->pipelineCycle = 0;
        publishMmioRegion();
        int n = publishUserRegions(stateKey_, regionsCsv_, out_, "VLACpuDelayAgent");
        if (verbose_ && n > 0)
            out_->output("VLACpuDelayAgent: published %d user region(s) into '%s'\n",
                         n, stateKey_.c_str());
    }

    if (verbose_) {
        const auto& cfg = fsm_.config();
        if (legacyScaling_) {
            out_->output("VLACpuDelayAgent: setup vit=%d llm=%d scale_factor=%.2f (legacy) max_seq=%d\n",
                         cfg.numViTLayers, cfg.numLLMLayers, scaleFactor_, cfg.maxSeqLen);
        } else {
            out_->output("VLACpuDelayAgent: setup vit=%d llm=%d scale_seq=%.2f scale_dim=%.2f scale_vocab=%.2f baseline_seq_len=%d max_seq=%d\n",
                         cfg.numViTLayers, cfg.numLLMLayers,
                         scaleSeq_, scaleDim_, scaleVocab_,
                         baselineSeqLen_, cfg.maxSeqLen);
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

void VLACpuDelayAgent::setRingLink(Link* leftLink)  { leftHaliLink_ = leftLink; }
void VLACpuDelayAgent::setInterceptBase(uint64_t b)  {
    controlAddrBase_ = b;
    if (!stateKey_.empty()) publishMmioRegion();
}
void VLACpuDelayAgent::setHighlink(Link* h)          { highlink_ = h; }

void VLACpuDelayAgent::publishMmioRegion()
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

// Implements the workload region-publish ABI declared in hyades.h. The binary
// writes base_lo/base_hi/size/commit to MMIO offsets 0x20/0x24/0x28/0x2C; we
// latch each into PipelineStateBase::stagedBase/stagedSize and commit on the
// COMMIT write. The committed (base, size) is the workload-virtual address
// range, which is what EccGuard matches against.
void VLACpuDelayAgent::applyRegionPublish(uint64_t offset, uint32_t value)
{
    if (stateKey_.empty()) return;
    PipelineStateBase* s =
        PipelineStateRegistry<PipelineStateBase>::getMutable(stateKey_);
    if (!s) s = PipelineStateRegistry<PipelineStateBase>::getOrCreate(stateKey_);
    switch (offset) {
    case 0x0020:
        s->stagedBase = (s->stagedBase & 0xFFFFFFFF00000000ull) |
                        static_cast<uint64_t>(value);
        break;
    case 0x0024:
        s->stagedBase = (s->stagedBase & 0x00000000FFFFFFFFull) |
                        (static_cast<uint64_t>(value) << 32);
        break;
    case 0x0028:
        s->stagedSize = static_cast<uint64_t>(value);
        break;
    case 0x002C: {
        size_t slot = static_cast<size_t>(value);
        // Slot 0 is reserved for mmio_control; refuse to overwrite it.
        if (slot == 0) {
            if (verbose_)
                out_->output("VLACpuDelayAgent: ignoring region commit to reserved slot 0\n");
            s->stagedBase = 0;
            s->stagedSize = 0;
            break;
        }
        // Preserve any symbolic name that came from the `regions` CSV.
        std::string preservedName;
        if (slot < s->regions.size())
            preservedName = s->regions[slot].name;
        s->commitStagedRegion(slot);
        if (!preservedName.empty())
            s->regions[slot].name = preservedName;
        if (verbose_)
            out_->output("VLACpuDelayAgent: region slot %zu '%s' published base=0x%" PRIx64 " size=0x%" PRIx64 "\n",
                         slot,
                         (slot < s->regions.size() ? s->regions[slot].name.c_str() : ""),
                         s->regions[slot].base, s->regions[slot].size);
        break;
    }
    default:
        break;
    }
}

void VLACpuDelayAgent::publishKernel(int kernel)
{
    if (stateKey_.empty()) return;
    PipelineStateBase* s =
        PipelineStateRegistry<PipelineStateBase>::getMutable(stateKey_);
    if (!s) s = PipelineStateRegistry<PipelineStateBase>::getOrCreate(stateKey_);
    s->currentKernel = kernel;
    s->pipelineCycle = fsm_.pipelineCycles();
}

void VLACpuDelayAgent::checkBothDone()
{
    if (!localDone_ || !partnerDone_) return;
    localDone_   = false;
    partnerDone_ = false;

    if (consumeFrameAbort(stateKey_)) {
        if (verbose_)
            out_->output("VLACpuDelayAgent: frame abort honored, fast-forwarding to ACTUATE\n");
        fsm_.fastForwardToActuate();
    }

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
    if (prev == ACTUATE && !stateKey_.empty()) {
        PipelineStateBase* s =
            PipelineStateRegistry<PipelineStateBase>::getMutable(stateKey_);
        if (s) {
            PipelineStateBase::FrameRecord fr;
            fr.pipelineCycle      = fsm_.pipelineCycles();
            fr.kernelAtClose      = static_cast<int>(prev);
            // Edge-triggered drop semantics: a frame is `dropped` iff the
            // pipeline-wide framesDropped counter advanced since the last
            // closed frame. The previous comparison against frames.size()
            // was a cumulative-vs-cumulative compare and produced misleading
            // per-frame counts whenever a single run had any drops.
            const int currentDrops = s->framesDropped;
            fr.dropped            = (currentDrops > lastFramesDroppedSeen_);
            lastFramesDroppedSeen_ = currentDrops;
            // Tier B (Fig. 3a) violation attribution: pick the kernel with
            // the largest in-frame escape count, then reset the map for
            // the next frame. argmax returns -1 when no escape fired this
            // frame (and the FrameRecord falls back to kernelAtClose at
            // the figure level).
            int attr = s->argmaxEccPerFrameEscapesByKernel();
            fr.attributingKernel  = (attr >= 0) ? attr : static_cast<int>(prev);
            s->resetEccPerFrameEscapesByKernel();
            // Prefer the workload-published checksum (Escape-sensitive: the
            // stub reads its action_queue back through the cache hierarchy
            // and folds it into 32 bits) if it arrived this frame; otherwise
            // fall back to a synthetic counter hash that at least varies
            // across frames so ActionScorer's golden file isn't all-zero.
            if (s->watcherActionChecksumValid) {
                fr.actionChecksum = s->watcherActionChecksum;
                s->watcherActionChecksumValid = false;
            } else if (latestActionChecksumSet_) {
                fr.actionChecksum = static_cast<uint64_t>(latestActionChecksum_);
                latestActionChecksumSet_ = false;
                latestActionChecksum_    = 0;
            } else {
                // Fallback: mix escape/flip counters into the hash so that
                // any frame experiencing an ECC escape diverges from the
                // golden (BER=0) checksum. Not as precise as the stub's
                // fold-XOR over action_queue memory, but escape-sensitive
                // without requiring async cache reads from this agent.
                fr.actionChecksum = static_cast<uint64_t>(fsm_.pipelineCycles())
                                    ^ (static_cast<uint64_t>(fsm_.currentSeqLen()) << 16)
                                    ^ (static_cast<uint64_t>(s->eccCumulativeEscapes) << 32)
                                    ^ static_cast<uint64_t>(s->eccCumulativeFlips);
            }
            fr.cumulativeEscapes  = s->eccCumulativeEscapes;
            fr.cumulativeFlips    = s->eccCumulativeFlips;
            fr.simTimePs          = static_cast<uint64_t>(getCurrentSimCycle());
            s->frames.push_back(fr);
        }
    }
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

    if (legacyScaling_)
        return computeLegacyScaledDelayPs(base, kernelId, scaleFactor_);

    int currentLayer = pickCurrentLayer(kernelId,
                                        fsm_.vitLayer(),
                                        fsm_.prefillLayer(),
                                        fsm_.decodeLayer());

    return computeScaledDelayPs(base, kernelId,
                                scaleSeq_, scaleDim_, scaleVocab_,
                                fsm_.currentSeqLen(),
                                baselineSeqLen_,
                                fsm_.actionTokenCount(),
                                currentLayer);
}

void VLACpuDelayAgent::sendCommandResponse(MemEvent* request, int value)
{
    publishKernel(value >= 0 ? value : KERNEL_IDLE);

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

    int dropped = 0;
    if (!stateKey_.empty()) {
        const PipelineStateBase* s =
            PipelineStateRegistry<PipelineStateBase>::get(stateKey_);
        if (s) dropped = s->framesDropped;
    }
    out_->output("=== VLA Frame Drops (CPU Delay Agent) ===\n");
    out_->output("frames_dropped\n%d\n", dropped);
    out_->output("=== End VLA Frame Drops ===\n\n");
}
