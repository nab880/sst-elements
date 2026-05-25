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
      latency_override_(0),
      handlers(nullptr),
      iface(nullptr),
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

    registerClock(tc_, new Clock::Handler<QuetzGpuDevice, &QuetzGpuDevice::tickBusy>(this));

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
}

void QuetzGpuDevice::setup() {
    iface->setup();
}

bool QuetzGpuDevice::isBusyAt(uint64_t now_clk) const {
    return busy_until_clk_ != 0 && now_clk <= busy_until_clk_;
}

void QuetzGpuDevice::retireIfReady(uint64_t now_clk) {
    if (busy_until_clk_ == 0 || now_clk < busy_until_clk_)
        return;

    kernel_id_++;
    busy_until_clk_ = 0;
    out.verbose(CALL_INFO, 2, 0,
        "%s: kernel %" PRIu64 " complete at clk %" PRIu64 "\n",
        getName().c_str(), kernel_id_, now_clk);

}

bool QuetzGpuDevice::tickBusy(Cycle_t /*cycle*/) {
    gpu_clk_++;
    uint64_t now_clk = gpu_clk_;

    if (busy_until_clk_ != 0 && now_clk <= busy_until_clk_)
        stat_busy_cycles_->addData(1);

    retireIfReady(now_clk);
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

    if (offset == REG_DOORBELL) {
        uint64_t latency = gpu->latency_override_;
        if (latency == 0)
            latency = gpu->kernel_latency_;
        gpu->latency_override_ = 0;

        while (gpu->isBusyAt(gpu->gpu_clk_)) {
            gpu->gpu_clk_++;
            gpu->stat_busy_cycles_->addData(1);
            gpu->retireIfReady(gpu->gpu_clk_);
        }

        uint64_t now_clk = gpu->gpu_clk_;
        gpu->stat_kernels_launched_->addData(1);
        if (latency == 0) {
            gpu->kernel_id_++;
            gpu->busy_until_clk_ = 0;
            out->verbose(CALL_INFO, 2, 0,
                "%s: doorbell — zero-latency kernel complete at clk %" PRIu64 "\n",
                gpu->getName().c_str(), now_clk);
        } else {
            gpu->busy_until_clk_ = now_clk + latency;
            out->verbose(CALL_INFO, 2, 0,
                "%s: doorbell — busy for %" PRIu64 " cycles (until %" PRIu64 ")\n",
                gpu->getName().c_str(), latency, gpu->busy_until_clk_);
        }
        gpu->stat_doorbell_writes_->addData(1);
    } else if (offset == REG_LATENCY_OVERRIDE) {
        gpu->latency_override_ = dataToU64(&write->data);
        gpu->stat_latency_overrides_->addData(1);
        out->verbose(CALL_INFO, 2, 0,
            "%s: latency_override=%" PRIu64 "\n",
            gpu->getName().c_str(), gpu->latency_override_);
    } else if (offset == REG_STATUS || offset == REG_KERNEL_ID) {
        gpu->stat_wrong_direction_accesses_->addData(1);
    } else {
        gpu->stat_bad_offset_accesses_->addData(1);
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
        value = gpu->isBusyAt(now_clk) ? 1 : 0;
        gpu->stat_status_polls_->addData(1);
    } else if (offset == REG_KERNEL_ID) {
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

void QuetzGpuDevice::printStatus(Output& statusOut) {
    statusOut.output("Quetz::QuetzGpuDevice %s\n", getName().c_str());
    statusOut.output("    base_addr=0x%" PRIx64 " mmio_size=0x%" PRIx64 "\n",
        base_addr_, mmio_size_);
    statusOut.output("    gpu_clk=%" PRIu64 " kernel_id=%" PRIu64
        " busy_until_clk=%" PRIu64 "\n",
        gpu_clk_, kernel_id_, busy_until_clk_);
    iface->printStatus(statusOut);
    statusOut.output("End Quetz::QuetzGpuDevice\n\n");
}
