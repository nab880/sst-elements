#include "sst_config.h"
#include "sst/elements/carcosa/VLA-Example/Components/VLAGpuAgent.h"
#include "sst/elements/carcosa/VLA-Example/Components/VLAAgent.h"
#include "sst/elements/carcosa/Components/HaliEvent.h"
#include "sst/elements/memHierarchy/memEvent.h"
#include "sst/elements/memHierarchy/memTypes.h"
#include <cinttypes>
#include <cstring>
#include <climits>

using namespace SST;
using namespace SST::MemHierarchy;
using namespace SST::Carcosa;

VLAGpuAgent::VLAGpuAgent(ComponentId_t id, Params& params)
    : InterceptionAgentAPI(id, params)
{
    out_ = new Output("", 1, 0, Output::STDOUT);
    maxSeqLen_   = params.find<int>("max_seq_len", 64);
    verbose_     = params.find<bool>("verbose", false);
    stateKey_    = params.find<std::string>("state_key", "");
    regionSize_  = params.find<uint64_t>("region_size", 4096);
}

VLAGpuAgent::~VLAGpuAgent()
{
    printProfile();
    delete out_;
}

bool VLAGpuAgent::handleInterceptedEvent(MemEvent* ev, Link* highlink)
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
        // Bump the local pipelineCycle the moment the GPU finishes an ACTUATE
        // kernel; matches the CPU FSM's pipelineCycles_ post-ACTUATE bump.
        if (activeKernelId_ == ACTUATE) gpuPipelineCycle_++;
        recordKernelEnd();
        publishKernel(KERNEL_IDLE);
        if (verbose_)
            out_->output("VLAGpuAgent: core done, sending done to CPU agent\n");
        if (leftHaliLink_)
            leftHaliLink_->send(new HaliEvent("done", 0u));
        return true;
    }

    return warnAndDropUnknownIntercept(ev, controlAddrBase_);
}

void VLAGpuAgent::handleRingEvent(HaliEvent* ev)
{
    const std::string& tag = ev->getStr();

    if (tag == "cmd") {
        nextCommand_ = static_cast<int>(ev->getNum());
        if (verbose_)
            out_->output("VLAGpuAgent: received cmd %d\n", nextCommand_);
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
                "VLAGpuAgent: received seqlen=%d from CPU agent but max_seq_len=%d; "
                "binary KV-cache would overflow. Raise max_seq_len or lower CPU initial_seq_len/num_action_tokens.\n",
                incoming, maxSeqLen_);
        }
        seqLen_ = incoming;
        if (verbose_)
            out_->output("VLAGpuAgent: received seqlen %d\n", seqLen_);
    } else if (tag == "exit") {
        nextCommand_ = -1;
        if (verbose_)
            out_->output("VLAGpuAgent: received exit\n");
        if (pendingCommandRead_) {
            activeKernelId_ = -1;
            sendCommandResponse(pendingCommandRead_, -1);
            pendingCommandRead_ = nullptr;
            nextCommand_ = INT_MIN;
        }
    }
}

void VLAGpuAgent::agentSetup()
{
    nextCommand_ = 0;
    seqLen_ = 0;
    gpuPipelineCycle_ = 0;

    if (!stateKey_.empty()) {
        PipelineStateBase* s =
            PipelineStateRegistry<PipelineStateBase>::getOrCreate(stateKey_);
        s->currentKernel = KERNEL_IDLE;
        s->pipelineCycle = 0;
        publishMmioRegion();
    }

    if (verbose_)
        out_->output("VLAGpuAgent: setup (initial cmd = IDLE) state_key=%s\n",
                     stateKey_.empty() ? "<unset>" : stateKey_.c_str());

    if (pendingCommandRead_) {
        activeKernelId_ = nextCommand_;
        kernelStartCycle_ = getCurrentSimCycle();
        sendCommandResponse(pendingCommandRead_, nextCommand_);
        pendingCommandRead_ = nullptr;
        nextCommand_ = INT_MIN;
    }
}

void VLAGpuAgent::setRingLink(Link* leftLink)  { leftHaliLink_ = leftLink; }
void VLAGpuAgent::setInterceptBase(uint64_t b)  {
    controlAddrBase_ = b;
    if (!stateKey_.empty()) publishMmioRegion();
}
void VLAGpuAgent::setHighlink(Link* h)          { highlink_ = h; }

void VLAGpuAgent::publishMmioRegion()
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

void VLAGpuAgent::publishKernel(int kernel)
{
    if (stateKey_.empty()) return;
    PipelineStateBase* s =
        PipelineStateRegistry<PipelineStateBase>::getMutable(stateKey_);
    if (!s) s = PipelineStateRegistry<PipelineStateBase>::getOrCreate(stateKey_);
    s->currentKernel = kernel;
    s->pipelineCycle = gpuPipelineCycle_;
}

void VLAGpuAgent::sendCommandResponse(MemEvent* request, int value)
{
    publishKernel(value >= 0 ? value : KERNEL_IDLE);

    MemEvent* resp = request->makeResponse();
    std::vector<uint8_t> data(4);
    std::memcpy(data.data(), &value, sizeof(int));
    resp->setPayload(data);
    if (highlink_) highlink_->send(resp);
    delete request;
}

void VLAGpuAgent::sendWriteAck(MemEvent* ev)
{
    MemEvent* resp = ev->makeResponse();
    if (highlink_) highlink_->send(resp);
    delete ev;
}

void VLAGpuAgent::recordKernelEnd()
{
    if (activeKernelId_ < 0) return;
    uint64_t endCycle = getCurrentSimCycle();
    profile_.push_back({"gpu", activeKernelId_, vlaStateName(activeKernelId_),
                        kernelStartCycle_, endCycle});
    activeKernelId_ = -1;
}

void VLAGpuAgent::printProfile()
{
    if (profile_.empty()) return;
    out_->output("\n=== VLA Per-Kernel Profile (GPU) ===\n");
    out_->output("core,kernel_id,kernel_name,start_ps,end_ps,delta_ps\n");
    for (auto& r : profile_) {
        out_->output("%s,%d,%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                     r.core.c_str(), r.kernelId, r.kernelName.c_str(),
                     r.startCycle, r.endCycle, r.endCycle - r.startCycle);
    }
    out_->output("=== End GPU Profile (%zu records) ===\n\n",
                 profile_.size());
}
