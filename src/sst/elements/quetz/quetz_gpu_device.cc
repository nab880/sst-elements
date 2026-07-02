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
#include "quetz_fft.h"

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
      kernel_type_(KernelType::NONE),
      fft_latency_coeff_(params.find<uint64_t>("fft_latency_coeff", 20)),
      mem_iface_(nullptr),
      fft_in_addr_(0),
      fft_out_addr_(0),
      fft_n_(0),
      fft_phase_(FftPhase::IDLE),
      fft_total_bytes_(0),
      fft_dma_outstanding_(0),
      fft_doorbell_resp_(nullptr),
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

    // kernel_type: 'none' (pure latency model, default) or 'fft' (real compute).
    std::string ktype = params.find<std::string>("kernel_type", "none");
    if (ktype == "none") {
        kernel_type_ = KernelType::NONE;
    } else if (ktype == "fft") {
        kernel_type_ = KernelType::FFT;
    } else {
        out.fatal(CALL_INFO, -1,
            "%s: unknown kernel_type '%s' (want 'none' or 'fft').\n",
            getName().c_str(), ktype.c_str());
    }

    if (kernel_type_ == KernelType::FFT) {
        // fft mode needs a memory initiator to DMA the guest buffers, and it must
        // hold the doorbell response until the result is written back.
        mem_iface_ = loadUserSubComponent<StandardMem>(
            "mem_iface", ComponentInfo::SHARE_NONE, tc_,
            new StandardMem::Handler<QuetzGpuDevice, &QuetzGpuDevice::handleEvent>(this));
        if (!mem_iface_) {
            out.fatal(CALL_INFO, -1,
                "%s: kernel_type=fft requires a 'mem_iface' subcomponent "
                "(memHierarchy.standardInterface) to DMA the FFT buffers.\n",
                getName().c_str());
        }
        if (!doorbell_blocking_) {
            out.fatal(CALL_INFO, -1,
                "%s: kernel_type=fft requires doorbell_blocking=1 (the guest must "
                "block until the result is in memory).\n", getName().c_str());
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
        || fft_phase_ != FftPhase::IDLE;
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

    // fft mode: the "BUSY" window models compute time; when it ends the result
    // is not yet in guest memory — kick off the writeback DMA. kernel_id and the
    // doorbell release happen in fftFinish() once the last WriteResp lands.
    if (kernel_type_ == KernelType::FFT && fft_phase_ == FftPhase::READING) {
        busy_until_clk_ = 0;
        fftBeginWriteback();
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

    // kernel_type=fft: the doorbell kicks off a real compute op (DMA-read the
    // input, compute in C++, DMA-write the result). The doorbell response is held
    // for the whole op so the guest's STATUS/blocking read only completes once the
    // result is in memory. One op in flight at a time.
    if (offset == REG_DOORBELL && gpu->kernel_type_ == QuetzGpuDevice::KernelType::FFT) {
        gpu->stat_doorbell_writes_->addData(1);
        if (gpu->fft_phase_ != QuetzGpuDevice::FftPhase::IDLE || gpu->isBusyAt(gpu->gpu_clk_)) {
            out->fatal(CALL_INFO, -1,
                "%s: fft doorbell while an FFT op is in flight (guest must "
                "wait for STATUS idle).\n", gpu->getName().c_str());
        }
        if (!write->posted)
            gpu->fft_doorbell_resp_ = write->makeResponse();
        gpu->submit_id_++;
        gpu->fftStartDma();
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
    } else if (offset == REG_FFT_IN_ADDR) {
        gpu->fft_in_addr_ = dataToU64(&write->data);
    } else if (offset == REG_FFT_OUT_ADDR) {
        gpu->fft_out_addr_ = dataToU64(&write->data);
    } else if (offset == REG_FFT_N) {
        gpu->fft_n_ = (uint32_t)dataToU64(&write->data);
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
        // An FFT op is busy for its whole doorbell-to-writeback lifetime, not
        // just the modeled-latency window (busy_until_clk_ is 0 during the DMA
        // read/write phases) — a poller must not see idle and re-ring mid-op.
        value = (gpu->isBusyAt(now_clk) ||
                 gpu->fft_phase_ != QuetzGpuDevice::FftPhase::IDLE) ? 1 : 0;
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

// --- mem_iface response handlers (fft DMA) -----------------------------------
void QuetzGpuDevice::mmioHandlers::handle(StandardMem::ReadResp* resp) {
    gpu->fftOnReadResp(resp);
}
void QuetzGpuDevice::mmioHandlers::handle(StandardMem::WriteResp* resp) {
    gpu->fftOnWriteResp(resp);
}

// --- kernel_type=fft: DMA + C++ FFT ------------------------------------------
//
// Sequence per doorbell: READING (DMA-read input) -> compute in C++ -> BUSY for
// the modeled latency -> WRITING (DMA-write result) -> fftFinish (release the
// held doorbell response). Buffers are little-endian float32 cfloat[N] (re,im).

// The radix-2 FFT math lives in quetz_fft.h (host-side, unit-tested); here we
// only marshal the little-endian float32 buffer to/from QuetzCf.
namespace {
static float le32_to_f32(const uint8_t* p) {
    uint32_t b = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
               | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    float f;
    memcpy(&f, &b, sizeof(f));
    return f;
}
static void f32_to_le32(float f, uint8_t* p) {
    uint32_t b;
    memcpy(&b, &f, sizeof(b));
    p[0] = (uint8_t)b; p[1] = (uint8_t)(b >> 8);
    p[2] = (uint8_t)(b >> 16); p[3] = (uint8_t)(b >> 24);
}
} // namespace

void QuetzGpuDevice::fftStartDma() {
    if (fft_n_ == 0 || (fft_n_ & (fft_n_ - 1)) != 0) {
        out.fatal(CALL_INFO, -1,
            "%s: fft REG_FFT_N=%" PRIu32 " must be a nonzero power of two.\n",
            getName().c_str(), fft_n_);
    }
    fft_total_bytes_ = (uint64_t)fft_n_ * sizeof(QuetzCf);   /* 8 bytes/point */
    fft_bytes_.assign(fft_total_bytes_, 0);
    fft_req_off_.clear();
    fft_dma_outstanding_ = 0;
    fft_phase_ = FftPhase::READING;

    out.verbose(CALL_INFO, 2, 0,
        "%s: fft doorbell — DMA-reading %" PRIu64 " bytes from 0x%" PRIx64 "\n",
        getName().c_str(), fft_total_bytes_, fft_in_addr_);

    for (uint64_t off = 0; off < fft_total_bytes_; off += kFftDmaChunk) {
        uint64_t n = fft_total_bytes_ - off;
        if (n > kFftDmaChunk) n = kFftDmaChunk;
        auto* rd = new StandardMem::Read(fft_in_addr_ + off, (size_t)n);
        fft_req_off_[rd->getID()] = off;
        fft_dma_outstanding_++;
        mem_iface_->send(rd);
    }
    updatePrimaryHold(false);
}

void QuetzGpuDevice::fftOnReadResp(StandardMem::ReadResp* resp) {
    auto it = fft_req_off_.find(resp->getID());
    if (it == fft_req_off_.end()) {
        out.fatal(CALL_INFO, -1, "%s: fft ReadResp with unknown id.\n",
            getName().c_str());
    }
    uint64_t off = it->second;
    fft_req_off_.erase(it);
    for (size_t i = 0; i < resp->data.size() && off + i < fft_total_bytes_; i++)
        fft_bytes_[off + i] = resp->data[i];
    if (--fft_dma_outstanding_ == 0)
        fftComputeAndStartBusy();
}

void QuetzGpuDevice::fftComputeAndStartBusy() {
    uint32_t n = fft_n_;
    uint32_t logn = 0;
    while ((1u << logn) < n) logn++;

    // unmarshal LE float32 -> host complex, run the radix-2 FFT (quetz_fft.h),
    // then marshal the result back into fft_bytes_ (reused for the writeback).
    std::vector<QuetzCf> a((size_t)n);
    for (uint32_t i = 0; i < n; i++) {
        a[i].re = le32_to_f32(&fft_bytes_[(size_t)i * 8 + 0]);
        a[i].im = le32_to_f32(&fft_bytes_[(size_t)i * 8 + 4]);
    }
    quetz_fft_radix2(a.data(), n);
    for (uint32_t i = 0; i < n; i++) {
        f32_to_le32(a[i].re, &fft_bytes_[(size_t)i * 8 + 0]);
        f32_to_le32(a[i].im, &fft_bytes_[(size_t)i * 8 + 4]);
    }

    // Model compute time: coeff * N * log2(N), unless the guest forced a latency.
    uint64_t latency = latency_override_;
    latency_override_ = 0;
    if (latency == 0)
        latency = fft_latency_coeff_ * (uint64_t)n * (uint64_t)(logn ? logn : 1);
    stat_kernels_launched_->addData(1);
    busy_until_clk_ = gpu_clk_ + latency;
    out.verbose(CALL_INFO, 2, 0,
        "%s: fft computed (N=%" PRIu32 "), BUSY %" PRIu64 " cycles then writeback\n",
        getName().c_str(), n, latency);
    updatePrimaryHold(false);
    // fftBeginWriteback() is invoked from retireIfReady() when BUSY ends.
}

void QuetzGpuDevice::fftBeginWriteback() {
    fft_phase_ = FftPhase::WRITING;
    fft_dma_outstanding_ = 0;
    out.verbose(CALL_INFO, 2, 0,
        "%s: fft DMA-writing %" PRIu64 " bytes to 0x%" PRIx64 "\n",
        getName().c_str(), fft_total_bytes_, fft_out_addr_);
    for (uint64_t off = 0; off < fft_total_bytes_; off += kFftDmaChunk) {
        uint64_t n = fft_total_bytes_ - off;
        if (n > kFftDmaChunk) n = kFftDmaChunk;
        std::vector<uint8_t> chunk(fft_bytes_.begin() + off,
                                   fft_bytes_.begin() + off + n);
        auto* wr = new StandardMem::Write(fft_out_addr_ + off, (size_t)n, chunk);
        fft_dma_outstanding_++;
        mem_iface_->send(wr);
    }
    updatePrimaryHold(false);
}

void QuetzGpuDevice::fftOnWriteResp(StandardMem::WriteResp* ) {
    if (fft_dma_outstanding_ == 0) return;
    if (--fft_dma_outstanding_ == 0)
        fftFinish();
}

void QuetzGpuDevice::fftFinish() {
    fft_phase_ = FftPhase::IDLE;
    kernel_id_++;
    out.verbose(CALL_INFO, 2, 0,
        "%s: fft op complete, result in memory (kernel_id=%" PRIu64 ")\n",
        getName().c_str(), kernel_id_);
    if (fft_doorbell_resp_) {
        iface->send(fft_doorbell_resp_);   // release the guest's blocking doorbell
        fft_doorbell_resp_ = nullptr;
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
