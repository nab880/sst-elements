// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#include <sst_config.h>
#include "quetz_gpu_device.h"

#include <cstring>
#include <inttypes.h>

using namespace SST;
using namespace SST::Interfaces;
using namespace SST::Quetz;

QuetzGpuDevice::QuetzGpuDevice(ComponentId_t id, Params& params)
    : Component(id),
      base_addr_(params.find<uint64_t>("base_addr", 0)),
      mmio_size_(params.find<uint64_t>("mmio_size", 0x400)),
      kernel_latency_(params.find<uint64_t>("kernel_latency", 5000)),
      gpu_clk_(0),
      busy_until_clk_(0),
      kernel_id_(0),
      submit_id_(0),
      latency_override_(0),
      holding_sim_(false),
      doorbell_blocking_(params.find<bool>("doorbell_blocking", false)),
      deferred_doorbell_resp_(nullptr),
      handlers(nullptr),
      iface(nullptr),
      kernel_(nullptr),
      mem_iface_(nullptr),
      arg_regs_{0, 0, 0, 0},
      op_args_{0, 0, 0, 0},
      op_phase_(OpPhase::IDLE),
      op_in_bytes_(0),
      op_dma_outstanding_(0),
      op_doorbell_resp_(nullptr),
      stat_kernels_launched_(nullptr),
      stat_busy_cycles_(nullptr),
      stat_doorbell_writes_(nullptr),
      stat_status_polls_(nullptr),
      stat_latency_overrides_(nullptr),
      stat_doorbell_while_busy_(nullptr),
      stat_wrong_direction_accesses_(nullptr),
      stat_bad_offset_accesses_(nullptr)
{
    out.init("", params.find<int>("verbose", 0), 0, Output::STDOUT);

    uint64_t dma_bytes = params.find<uint64_t>("dma_bytes_per_kernel", 0);
    if (dma_bytes != 0) {
        out.fatal(CALL_INFO, -1,
            "%s: dma_bytes_per_kernel=%" PRIu64 " is not supported in P2.a "
            "(reserved for P2.b shared-bus DMA).\n",
            getName().c_str(), dma_bytes);
    }

    std::string clockfreq = params.find<std::string>("clock", "1GHz");
    UnitAlgebra clock_ua(clockfreq);
    if (!(clock_ua.hasUnits("Hz") || clock_ua.hasUnits("s")) ||
        clock_ua.getRoundedValue() <= 0) {
        out.fatal(CALL_INFO, -1,
            "%s: invalid clock '%s' (must be Hz or s, > 0).\n",
            getName().c_str(), clockfreq.c_str());
    }
    tc_ = getTimeConverter(clockfreq);

    iface = loadUserSubComponent<StandardMem>(
        "iface", ComponentInfo::SHARE_NONE, tc_,
        new StandardMem::Handler<QuetzGpuDevice, &QuetzGpuDevice::handleEvent>(this));

    if (!iface) {
        out.fatal(CALL_INFO, -1,
            "%s: no 'iface' subcomponent; load memHierarchy.standardInterface.\n",
            getName().c_str());
    }

    iface->setMemoryMappedAddressRegion(base_addr_, mmio_size_);

    handlers = new mmioHandlers(this, &out);

    // 'kernel' slot: populated = real compute per doorbell; empty = pure
    // latency model.
    kernel_ = loadUserSubComponent<QuetzKernel>("kernel");

    if (kernel_) {
        // A kernel needs a memory initiator to DMA the guest buffers, and it
        // must hold the doorbell response until the result is written back.
        mem_iface_ = loadUserSubComponent<StandardMem>(
            "mem_iface", ComponentInfo::SHARE_NONE, tc_,
            new StandardMem::Handler<QuetzGpuDevice, &QuetzGpuDevice::handleEvent>(this));
        if (!mem_iface_) {
            out.fatal(CALL_INFO, -1,
                "%s: a 'kernel' subcomponent requires a 'mem_iface' subcomponent "
                "(memHierarchy.standardInterface) to DMA the kernel buffers.\n",
                getName().c_str());
        }
        if (!doorbell_blocking_) {
            out.fatal(CALL_INFO, -1,
                "%s: a 'kernel' subcomponent requires doorbell_blocking=1 (the "
                "guest must block until the result is in memory).\n",
                getName().c_str());
        }
    }

    registerClock(tc_, new Clock::Handler<QuetzGpuDevice, &QuetzGpuDevice::tickBusy>(this));

    registerAsPrimaryComponent();
    primaryComponentDoNotEndSim();
    holding_sim_ = true;

    stat_kernels_launched_    = registerStatistic<uint64_t>("kernels_launched");
    stat_busy_cycles_         = registerStatistic<uint64_t>("busy_cycles");
    stat_doorbell_writes_     = registerStatistic<uint64_t>("doorbell_writes");
    stat_status_polls_        = registerStatistic<uint64_t>("status_polls");
    stat_latency_overrides_   = registerStatistic<uint64_t>("latency_overrides");
    stat_doorbell_while_busy_ = registerStatistic<uint64_t>("doorbell_while_busy");
    stat_wrong_direction_accesses_ =
        registerStatistic<uint64_t>("wrong_direction_accesses");
    stat_bad_offset_accesses_ = registerStatistic<uint64_t>("bad_offset_accesses");

    out.verbose(CALL_INFO, 1, 0,
        "%s: MMIO [0x%" PRIx64 ", 0x%" PRIx64 ") kernel_latency=%" PRIu64 " cycles\n",
        getName().c_str(), base_addr_, base_addr_ + mmio_size_, kernel_latency_);
}

void QuetzGpuDevice::init(unsigned int phase) {
    iface->init(phase);
    if (mem_iface_)
        mem_iface_->init(phase);
}

void QuetzGpuDevice::setup() {
    iface->setup();
    if (mem_iface_)
        mem_iface_->setup();
}

bool QuetzGpuDevice::isBusyAt(uint64_t ) const {
    return busy_until_clk_ != 0;
}

bool QuetzGpuDevice::hasOutstandingWork() const {
    return busy_until_clk_ != 0 || !pending_latencies_.empty()
        || op_phase_ != OpPhase::IDLE;
}

void QuetzGpuDevice::startKernel(uint64_t now_clk, uint64_t latency) {
    stat_kernels_launched_->addData(1);
    if (latency == 0) {
        kernel_id_++;
        busy_until_clk_ = 0;
        out.verbose(CALL_INFO, 2, 0,
            "%s: kernel %" PRIu64 " complete at clk %" PRIu64 " (zero latency)\n",
            getName().c_str(), kernel_id_, now_clk);
    } else {
        busy_until_clk_ = now_clk + latency;
        out.verbose(CALL_INFO, 2, 0,
            "%s: doorbell — busy for %" PRIu64 " cycles (until %" PRIu64 ")\n",
            getName().c_str(), latency, busy_until_clk_);
    }
    updatePrimaryHold(false);
}

void QuetzGpuDevice::updatePrimaryHold(bool allow_ok_to_end) {
    if (hasOutstandingWork()) {
        if (!holding_sim_) {
            primaryComponentDoNotEndSim();
            holding_sim_ = true;
        }
    } else if (holding_sim_ && allow_ok_to_end) {
        primaryComponentOKToEndSim();
        holding_sim_ = false;
    }
}

void QuetzGpuDevice::retireIfReady(uint64_t now_clk) {
    if (busy_until_clk_ == 0 || now_clk <= busy_until_clk_)
        return;

    // kernel op: the "BUSY" window models compute time; when it ends the result
    // is not yet in guest memory — kick off the writeback DMA. kernel_id and the
    // doorbell release happen in opFinish() once the last WriteResp lands.
    if (kernel_ && op_phase_ == OpPhase::READING) {
        busy_until_clk_ = 0;
        opBeginWriteback();
        return;
    }

    kernel_id_++;
    busy_until_clk_ = 0;
    out.verbose(CALL_INFO, 2, 0,
        "%s: kernel %" PRIu64 " complete at clk %" PRIu64 "\n",
        getName().c_str(), kernel_id_, now_clk);

    // doorbell_blocking_: release the held doorbell write response now that the
    // kernel it launched has finished (mimics balar's deferred blocked_response).
    if (deferred_doorbell_resp_) {
        iface->send(deferred_doorbell_resp_);
        deferred_doorbell_resp_ = nullptr;
    }

    if (!pending_latencies_.empty()) {
        uint64_t latency = pending_latencies_.front();
        pending_latencies_.pop_front();
        startKernel(now_clk, latency);
    }
}

bool QuetzGpuDevice::tickBusy(Cycle_t ) {
    gpu_clk_++;
    uint64_t now_clk = gpu_clk_;

    if (busy_until_clk_ != 0)
        stat_busy_cycles_->addData(1);

    retireIfReady(now_clk);
    updatePrimaryHold(true);
    return false;
}

void QuetzGpuDevice::handleEvent(StandardMem::Request* req) {
    req->handle(handlers);
    delete req;
}

void QuetzGpuDevice::mmioHandlers::u64ToData(
    uint64_t val, std::vector<uint8_t>* data, size_t size)
{
    data->clear();
    for (size_t i = 0; i < size; i++) {
        data->push_back(static_cast<uint8_t>(val & 0xFF));
        val >>= 8;
    }
}

uint64_t QuetzGpuDevice::mmioHandlers::dataToU64(std::vector<uint8_t>* data) {
    uint64_t retval = 0;
    for (int i = static_cast<int>(data->size()) - 1; i >= 0; i--) {
        retval <<= 8;
        retval |= (*data)[static_cast<size_t>(i)];
    }
    return retval;
}

void QuetzGpuDevice::mmioHandlers::handle(StandardMem::Write* write) {
    uint64_t offset = write->pAddr - gpu->base_addr_;

    out->verbose(CALL_INFO, 2, 0,
        "%s: Write offset=0x%" PRIx64 " size=%zu\n",
        gpu->getName().c_str(), offset, write->size);

    // Kernel slot populated: the doorbell kicks off a real compute op (DMA-read
    // the input, run the kernel, DMA-write the result). The doorbell response is
    // held for the whole op so the guest's STATUS/blocking read only completes
    // once the result is in memory. One op in flight at a time.
    if (offset == REG_DOORBELL && gpu->kernel_) {
        gpu->stat_doorbell_writes_->addData(1);
        if (gpu->op_phase_ != QuetzGpuDevice::OpPhase::IDLE || gpu->isBusyAt(gpu->gpu_clk_)) {
            out->fatal(CALL_INFO, -1,
                "%s: doorbell while a kernel op is in flight (guest must "
                "wait for STATUS idle).\n", gpu->getName().c_str());
        }
        if (!write->posted)
            gpu->op_doorbell_resp_ = write->makeResponse();
        gpu->submit_id_++;
        gpu->op_args_ = { gpu->arg_regs_[0], gpu->arg_regs_[1],
                          gpu->arg_regs_[2], gpu->arg_regs_[3] };
        gpu->opStartDma();
        return;
    }

    if (offset == REG_DOORBELL) {
        uint64_t latency = gpu->latency_override_;
        if (latency == 0)
            latency = gpu->kernel_latency_;
        gpu->latency_override_ = 0;

        gpu->retireIfReady(gpu->gpu_clk_);

        if (!gpu->isBusyAt(gpu->gpu_clk_) && gpu->pending_latencies_.empty()) {
            gpu->submit_id_++;
            gpu->startKernel(gpu->gpu_clk_, latency);
        } else if (gpu->pending_latencies_.size() < gpu->kMaxPendingLaunches) {
            gpu->submit_id_++;
            gpu->pending_latencies_.push_back(latency);
            gpu->stat_doorbell_while_busy_->addData(1);
            gpu->updatePrimaryHold(false);
            out->verbose(CALL_INFO, 2, 0,
                "%s: doorbell queued — ticket %" PRIu64 " latency %" PRIu64 " cycles\n",
                gpu->getName().c_str(), gpu->submit_id_, latency);
        } else {
            gpu->stat_doorbell_while_busy_->addData(1);
            out->verbose(CALL_INFO, 2, 0,
                "%s: doorbell dropped (queue full)\n",
                gpu->getName().c_str());
        }
        gpu->stat_doorbell_writes_->addData(1);
    } else if (offset == REG_LATENCY_OVERRIDE) {
        gpu->latency_override_ = dataToU64(&write->data);
        gpu->stat_latency_overrides_->addData(1);
        out->verbose(CALL_INFO, 2, 0,
            "%s: latency_override=%" PRIu64 "\n",
            gpu->getName().c_str(), gpu->latency_override_);
    } else if (offset == REG_ARG0) {
        gpu->arg_regs_[0] = dataToU64(&write->data);
    } else if (offset == REG_ARG1) {
        gpu->arg_regs_[1] = dataToU64(&write->data);
    } else if (offset == REG_ARG2) {
        gpu->arg_regs_[2] = dataToU64(&write->data);
    } else if (offset == REG_ARG3) {
        gpu->arg_regs_[3] = dataToU64(&write->data);
    } else if (offset == REG_STATUS || offset == REG_KERNEL_ID ||
               offset == REG_TICKET || offset == REG_RESULT) {
        gpu->stat_wrong_direction_accesses_->addData(1);
    } else {
        gpu->stat_bad_offset_accesses_->addData(1);
    }

    // In doorbell_blocking_ mode, hold the doorbell write response until the
    // kernel it launched retires; everything else acks immediately.
    if (offset == REG_DOORBELL && gpu->doorbell_blocking_ &&
        gpu->isBusyAt(gpu->gpu_clk_) && !write->posted) {
        if (gpu->deferred_doorbell_resp_) {
            out->fatal(CALL_INFO, -1,
                "%s: doorbell while a blocking doorbell is outstanding "
                "(expected one in-flight op).\n", gpu->getName().c_str());
        }
        gpu->deferred_doorbell_resp_ = write->makeResponse();
        return;
    }

    if (!write->posted)
        gpu->iface->send(write->makeResponse());
}

void QuetzGpuDevice::mmioHandlers::handle(StandardMem::Read* read) {
    uint64_t offset = read->pAddr - gpu->base_addr_;
    uint64_t value = 0;
    uint64_t now_clk = gpu->gpu_clk_;

    out->verbose(CALL_INFO, 2, 0,
        "%s: Read offset=0x%" PRIx64 " size=%zu\n",
        gpu->getName().c_str(), offset, read->size);

    if (offset == REG_STATUS) {
        gpu->retireIfReady(now_clk);
        // A kernel op is busy for its whole doorbell-to-writeback lifetime, not
        // just the modeled-latency window (busy_until_clk_ is 0 during the DMA
        // read/write phases) — a poller must not see idle and re-ring mid-op.
        value = (gpu->isBusyAt(now_clk) ||
                 gpu->op_phase_ != QuetzGpuDevice::OpPhase::IDLE) ? 1 : 0;
        gpu->stat_status_polls_->addData(1);
    } else if (offset == REG_KERNEL_ID) {
        gpu->retireIfReady(now_clk);
        value = gpu->kernel_id_;
    } else if (offset == REG_TICKET) {
        value = gpu->submit_id_;
    } else if (offset == REG_RESULT) {
        // Synthetic device has no real kernel output, so RESULT mirrors the
        // completed-kernel counter (same as REG_KERNEL_ID) — enough for the
        // balar-free async/completion tests that only check it advances.
        gpu->retireIfReady(now_clk);
        value = gpu->kernel_id_;
    } else if (offset == REG_DOORBELL || offset == REG_LATENCY_OVERRIDE) {
        gpu->stat_wrong_direction_accesses_->addData(1);
        value = 0;
    } else {
        gpu->stat_bad_offset_accesses_->addData(1);
        value = 0;
    }

    std::vector<uint8_t> payload;
    u64ToData(value, &payload, read->size);

    StandardMem::ReadResp* resp =
        static_cast<StandardMem::ReadResp*>(read->makeResponse());
    resp->data = payload;
    gpu->iface->send(resp);
}

// --- mem_iface response handlers (kernel DMA) ---------------------------------
void QuetzGpuDevice::mmioHandlers::handle(StandardMem::ReadResp* resp) {
    gpu->opOnReadResp(resp);
}
void QuetzGpuDevice::mmioHandlers::handle(StandardMem::WriteResp* resp) {
    gpu->opOnWriteResp(resp);
}

// --- kernel-slot ops: DMA around the plugged compute ---------------------------
//
// Sequence per doorbell: READING (DMA-read kernel_->inputBytes() from ARG0) ->
// kernel_->compute() -> BUSY for the modeled latency -> WRITING (DMA-write the
// kernel's output to ARG1) -> opFinish (release the held doorbell response).
// Data format and latency model are the kernel's business; the device only
// moves bytes.

void QuetzGpuDevice::opStartDma() {
    std::string err;
    op_in_bytes_ = kernel_->inputBytes(op_args_, err);
    if (op_in_bytes_ == 0) {
        out.fatal(CALL_INFO, -1, "%s: kernel rejected the op: %s\n",
            getName().c_str(), err.empty() ? "(no reason given)" : err.c_str());
    }
    op_in_.assign(op_in_bytes_, 0);
    op_req_off_.clear();
    op_dma_outstanding_ = 0;
    op_phase_ = OpPhase::READING;

    out.verbose(CALL_INFO, 2, 0,
        "%s: doorbell — DMA-reading %" PRIu64 " bytes from 0x%" PRIx64 "\n",
        getName().c_str(), op_in_bytes_, op_args_.src_addr);

    for (uint64_t off = 0; off < op_in_bytes_; off += kOpDmaChunk) {
        uint64_t n = op_in_bytes_ - off;
        if (n > kOpDmaChunk) n = kOpDmaChunk;
        auto* rd = new StandardMem::Read(op_args_.src_addr + off, (size_t)n);
        op_req_off_[rd->getID()] = off;
        op_dma_outstanding_++;
        mem_iface_->send(rd);
    }
    updatePrimaryHold(false);
}

void QuetzGpuDevice::opOnReadResp(StandardMem::ReadResp* resp) {
    auto it = op_req_off_.find(resp->getID());
    if (it == op_req_off_.end()) {
        out.fatal(CALL_INFO, -1, "%s: kernel-DMA ReadResp with unknown id.\n",
            getName().c_str());
    }
    uint64_t off = it->second;
    op_req_off_.erase(it);
    for (size_t i = 0; i < resp->data.size() && off + i < op_in_bytes_; i++)
        op_in_[off + i] = resp->data[i];
    if (--op_dma_outstanding_ == 0)
        opComputeAndStartBusy();
}

void QuetzGpuDevice::opComputeAndStartBusy() {
    op_out_.clear();
    uint64_t kernel_latency = kernel_->compute(op_args_, op_in_, op_out_);
    if (op_out_.empty()) {
        out.fatal(CALL_INFO, -1,
            "%s: kernel produced no output bytes.\n", getName().c_str());
    }

    // Latency precedence: guest override > kernel opinion > device default.
    uint64_t latency = latency_override_;
    latency_override_ = 0;
    if (latency == 0)
        latency = kernel_latency ? kernel_latency : kernel_latency_;
    stat_kernels_launched_->addData(1);
    busy_until_clk_ = gpu_clk_ + latency;
    out.verbose(CALL_INFO, 2, 0,
        "%s: kernel computed (%zu -> %zu bytes), BUSY %" PRIu64
        " cycles then writeback\n",
        getName().c_str(), op_in_.size(), op_out_.size(), latency);
    updatePrimaryHold(false);
    // opBeginWriteback() is invoked from retireIfReady() when BUSY ends.
}

void QuetzGpuDevice::opBeginWriteback() {
    op_phase_ = OpPhase::WRITING;
    op_dma_outstanding_ = 0;
    out.verbose(CALL_INFO, 2, 0,
        "%s: DMA-writing %zu bytes to 0x%" PRIx64 "\n",
        getName().c_str(), op_out_.size(), op_args_.dst_addr);
    for (uint64_t off = 0; off < op_out_.size(); off += kOpDmaChunk) {
        uint64_t n = op_out_.size() - off;
        if (n > kOpDmaChunk) n = kOpDmaChunk;
        std::vector<uint8_t> chunk(op_out_.begin() + off,
                                   op_out_.begin() + off + n);
        auto* wr = new StandardMem::Write(op_args_.dst_addr + off, (size_t)n, chunk);
        op_dma_outstanding_++;
        mem_iface_->send(wr);
    }
    updatePrimaryHold(false);
}

void QuetzGpuDevice::opOnWriteResp(StandardMem::WriteResp* ) {
    if (op_dma_outstanding_ == 0) return;
    if (--op_dma_outstanding_ == 0)
        opFinish();
}

void QuetzGpuDevice::opFinish() {
    op_phase_ = OpPhase::IDLE;
    kernel_id_++;
    out.verbose(CALL_INFO, 2, 0,
        "%s: kernel op complete, result in memory (kernel_id=%" PRIu64 ")\n",
        getName().c_str(), kernel_id_);
    if (op_doorbell_resp_) {
        iface->send(op_doorbell_resp_);    // release the guest's blocking doorbell
        op_doorbell_resp_ = nullptr;
    }
    updatePrimaryHold(true);
}

void QuetzGpuDevice::printStatus(Output& statusOut) {
    statusOut.output("Quetz::QuetzGpuDevice %s\n", getName().c_str());
    statusOut.output("    base_addr=0x%" PRIx64 " mmio_size=0x%" PRIx64 "\n",
        base_addr_, mmio_size_);
    statusOut.output("    gpu_clk=%" PRIu64 " kernel_id=%" PRIu64
        " busy_until_clk=%" PRIu64 " pending=%zu holding_sim=%d\n",
        gpu_clk_, kernel_id_, busy_until_clk_, pending_latencies_.size(),
        holding_sim_ ? 1 : 0);
    iface->printStatus(statusOut);
    statusOut.output("End Quetz::QuetzGpuDevice\n\n");
}
