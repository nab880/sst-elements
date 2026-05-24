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
#include "quetzcpu.h"

#include "quetz_config_manager.h"

#include <inttypes.h>

using namespace SST;
using namespace SST::Quetz;
using namespace SST::Interfaces;

QuetzCPU::QuetzCPU(ComponentId_t id, Params& params)
    : Component(id),
      output_(new SST::Output(
          "QuetzComponent[@f:@l:@p] ", 0, 0, SST::Output::STDOUT)),
      frontend_(nullptr),
      stop_ticking_(true),
      halted_count_(0)
{
    cfg_ = QuetzConfigManager::fromParams(params, output_).config();
    output_->setVerboseLevel(cfg_.verbosity);

    output_->verbose(CALL_INFO, 1, 0, "Creating QuetzComponent...\n");
    output_->verbose(CALL_INFO, 1, 0,
        "Configuring for %" PRIu32 " vCPU(s).\n", cfg_.vcpu_count);

    loadRegionHandlers();
    region_table_ = MemRegionTable(region_handlers_);

    frontend_ = new QemuFrontend(id, cfg_.vcpu_count, cfg_.max_core_queue, output_);

    output_->verbose(CALL_INFO, 1, 0,
        "Shared-memory region: %s\n",
        frontend_->shmemRegionName().c_str());

    output_->verbose(CALL_INFO, 1, 0,
        "Registering clock at %s.\n", cfg_.cpu_clock.c_str());

    TimeConverter tc = registerClock(
        cfg_.cpu_clock,
        new Clock::Handler<QuetzCPU, &QuetzCPU::tick>(this));

    output_->verbose(CALL_INFO, 1, 0, "Creating per-vCPU cores...\n");

    for (uint32_t i = 0; i < cfg_.vcpu_count; i++) {
        cores_.push_back(loadComponentExtension<QuetzCore>(
            frontend_->coreBackend(), i, output_, tc, params,
            &region_table_, cfg_.max_pend_trans, cfg_.max_issue_cyc,
            cfg_.max_core_queue, cfg_.cache_line_sz, cfg_.max_insts,
            cfg_.check_addresses, cfg_.detailed_tracking,
            cfg_.exec_latency, cfg_.compute_latency));
    }

    SubComponentSlotInfo* mem_slot = getSubComponentSlotInfo("memory");
    if (mem_slot) {
        if (!mem_slot->isAllPopulated() ||
            (uint32_t)mem_slot->getMaxPopulatedSlotNumber() != cfg_.vcpu_count - 1)
        {
            output_->fatal(CALL_INFO, -1,
                "Mismatch: 'memory' subcomponent slots must match vcpu_count "
                "(%" PRIu32 " vCPUs).\n", cfg_.vcpu_count);
        }
        for (uint32_t i = 0; i < cfg_.vcpu_count; i++) {
            auto* iface = mem_slot->create<StandardMem>(i,
                ComponentInfo::INSERT_STATS, tc,
                new StandardMem::Handler<QuetzCore,
                    &QuetzCore::handleMemResponse>(cores_[i]));
            mem_ifaces_.push_back(iface);
            cores_[i]->setMemLink(iface);
        }
    } else {
        char link_buf[128];
        for (uint32_t i = 0; i < cfg_.vcpu_count; i++) {
            Params par;
            snprintf(link_buf, sizeof(link_buf), "cache_link_%" PRIu32, i);
            par.insert("port", std::string(link_buf));
            auto* iface = loadAnonymousSubComponent<StandardMem>(
                "memHierarchy.standardInterface", "memory", i,
                ComponentInfo::SHARE_PORTS | ComponentInfo::INSERT_STATS,
                par, tc,
                new StandardMem::Handler<QuetzCore,
                    &QuetzCore::handleMemResponse>(cores_[i]));
            mem_ifaces_.push_back(iface);
            cores_[i]->setMemLink(iface);
        }
    }

    mmio_ifaces_.assign(cfg_.vcpu_count, nullptr);
    SubComponentSlotInfo* mmio_slot = getSubComponentSlotInfo("mmio");
    for (uint32_t i = 0; i < cfg_.vcpu_count; i++) {
        char link_buf[128];
        snprintf(link_buf, sizeof(link_buf), "mmio_link_%" PRIu32, i);
        bool want_mmio = isPortConnected(link_buf);
        if (!want_mmio && mmio_slot && mmio_slot->isPopulated(i))
            want_mmio = true;
        if (!want_mmio)
            continue;

        StandardMem* iface = nullptr;
        if (mmio_slot && mmio_slot->isPopulated(i)) {
            iface = mmio_slot->create<StandardMem>(i,
                ComponentInfo::INSERT_STATS, tc,
                new StandardMem::Handler<QuetzCore,
                    &QuetzCore::handleMemResponse>(cores_[i]));
        } else {
            Params par;
            par.insert("port", std::string(link_buf));
            iface = loadAnonymousSubComponent<StandardMem>(
                "memHierarchy.standardInterface", "mmio", i,
                ComponentInfo::SHARE_PORTS | ComponentInfo::INSERT_STATS,
                par, tc,
                new StandardMem::Handler<QuetzCore,
                    &QuetzCore::handleMemResponse>(cores_[i]));
        }
        mmio_ifaces_[i] = iface;
        cores_[i]->setMmioLink(iface);
        output_->verbose(CALL_INFO, 1, 0,
            "vCPU %" PRIu32 ": mmio_link connected.\n", i);
    }

    registerAsPrimaryComponent();
    primaryComponentDoNotEndSim();

    output_->verbose(CALL_INFO, 1, 0,
        "QuetzComponent initialization complete.\n");
}

void QuetzCPU::loadRegionHandlers() {
    region_handlers_.clear();

    SubComponentSlotInfo* rh_slot = getSubComponentSlotInfo("region_handler");
    if (rh_slot && rh_slot->getMaxPopulatedSlotNumber() >= 0) {
        for (uint32_t s = 0; s <= rh_slot->getMaxPopulatedSlotNumber(); s++) {
            auto* h = rh_slot->create<MemRegionHandler>(
                s, ComponentInfo::INSERT_STATS);
            region_handlers_.push_back(h);
            output_->verbose(CALL_INFO, 1, 0,
                "region_handler[%" PRIu32 "]: 0x%016" PRIx64 "-0x%016" PRIx64 "\n",
                s, h->startAddr(), h->endAddr());
        }
        return;
    }

    for (size_t i = 0; i < cfg_.region_handlers.size(); i++) {
        const RegionHandlerPreset& preset = cfg_.region_handlers[i];
        Params p;
        for (const auto& kv : preset.params)
            p.insert(kv.first, kv.second);
        auto* h = loadAnonymousSubComponent<MemRegionHandler>(
            preset.type, "region_handler", (int)i,
            ComponentInfo::INSERT_STATS, p);
        region_handlers_.push_back(h);
        output_->verbose(CALL_INFO, 1, 0,
            "region_handler preset[%" PRIu32 "]: %s "
            "0x%016" PRIx64 "-0x%016" PRIx64 "\n",
            (uint32_t)i, preset.type.c_str(), h->startAddr(), h->endAddr());
    }
}

QuetzCPU::~QuetzCPU() {
    delete frontend_;
    delete output_;
}

void QuetzCPU::init(unsigned int phase) {
    if (phase == 0) {
        output_->verbose(CALL_INFO, 1, 0,
            "Phase 0 init: launching QEMU child process.\n");
        frontend_->spawn(cfg_, cfg_.detailed_tracking);
        output_->verbose(CALL_INFO, 1, 0,
            "Waiting for QEMU plugin to attach...\n");
        frontend_->waitForChildAttach();
        output_->verbose(CALL_INFO, 1, 0, "Plugin attached!\n");
        stop_ticking_ = false;
    }

    for (uint32_t i = 0; i < cfg_.vcpu_count; i++) {
        mem_ifaces_[i]->init(phase);
        if (mmio_ifaces_[i])
            mmio_ifaces_[i]->init(phase);
    }
}

void QuetzCPU::finish() {
    output_->verbose(CALL_INFO, 1, 0,
        "QuetzComponent finishing at %" PRIu64 " ns.\n",
        (uint64_t)getCurrentSimTimeNano());

    for (uint32_t i = 0; i < cfg_.vcpu_count; i++)
        cores_[i]->finishCore();

    frontend_->terminate();
}

void QuetzCPU::emergencyShutdown() {
    frontend_->forceKill();
    delete frontend_;
    frontend_ = nullptr;
}

bool QuetzCPU::tick(SST::Cycle_t /*cycle*/) {
    QuetzCoreBackend* backend = frontend_->coreBackend();
    backend->updateSimTime(getCurrentSimTimeNano());
    backend->incrementCycles();

    for (uint32_t i = 0; i < cfg_.vcpu_count; i++) {
        bool was_halted = cores_[i]->isCoreHalted();
        cores_[i]->tick();
        if (!was_halted && cores_[i]->isCoreHalted())
            halted_count_++;
    }

    if (halted_count_ < cfg_.vcpu_count)
        return false;

    for (uint32_t i = 0; i < cfg_.vcpu_count; i++)
        if (cores_[i]->pendingCount() > 0) return false;

    if (!stop_ticking_) {
        output_->verbose(CALL_INFO, 1, 0,
            "All %" PRIu32 " vCPUs halted and drained — ending simulation.\n",
            cfg_.vcpu_count);
        primaryComponentOKToEndSim();
        stop_ticking_ = true;
    }
    return true;
}
