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

#include <inttypes.h>

using namespace SST;
using namespace SST::Quetz;
using namespace SST::Interfaces;

QuetzCPU::QuetzCPU(ComponentId_t id, Params& params)
    : Component(id),
      output_(new SST::Output(
          "QuetzComponent[@f:@l:@p] ", 0, 0, SST::Output::STDOUT)),
      tunnelmgr_(nullptr),
      tunnel_(nullptr),
      launcher_(output_),
      stop_ticking_(true),
      halted_count_(0)
{
    cfg_ = QuetzConfig::fromParams(params, output_);
    output_->setVerboseLevel(cfg_.verbosity);

    output_->verbose(CALL_INFO, 1, 0, "Creating QuetzComponent...\n");
    output_->verbose(CALL_INFO, 1, 0,
        "Configuring for %" PRIu32 " vCPU(s).\n", cfg_.vcpu_count);

    tunnelmgr_ = new SST::Core::Interprocess::SHMParent<QuetzTunnel>(
        id, cfg_.vcpu_count, cfg_.max_core_queue);
    tunnel_ = tunnelmgr_->getTunnel();

    output_->verbose(CALL_INFO, 1, 0,
        "Shared-memory region: %s\n",
        tunnelmgr_->getRegionName().c_str());

    output_->verbose(CALL_INFO, 1, 0,
        "Registering clock at %s.\n", cfg_.cpu_clock.c_str());

    TimeConverter tc = registerClock(
        cfg_.cpu_clock,
        new Clock::Handler<QuetzCPU, &QuetzCPU::tick>(this));

    output_->verbose(CALL_INFO, 1, 0, "Creating per-vCPU cores...\n");

    for (uint32_t i = 0; i < cfg_.vcpu_count; i++) {
        cores_.push_back(loadComponentExtension<QuetzCore>(
            tunnel_, i, cfg_.max_pend_trans, output_,
            cfg_.max_issue_cyc, cfg_.max_core_queue,
            cfg_.cache_line_sz, tc, params,
            cfg_.exec_latency, cfg_.compute_latency, cfg_.memmap,
            cfg_.max_insts, cfg_.check_addresses, cfg_.detailed_tracking));
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

    registerAsPrimaryComponent();
    primaryComponentDoNotEndSim();

    output_->verbose(CALL_INFO, 1, 0,
        "QuetzComponent initialization complete.\n");
}

QuetzCPU::~QuetzCPU() {
    delete tunnelmgr_;
    delete output_;
}

void QuetzCPU::init(unsigned int phase) {
    if (phase == 0) {
        output_->verbose(CALL_INFO, 1, 0,
            "Phase 0 init: launching QEMU child process.\n");
        launcher_.spawn(cfg_, tunnelmgr_->getRegionName(), cfg_.detailed_tracking);
        output_->verbose(CALL_INFO, 1, 0,
            "Waiting for QEMU plugin to attach...\n");
        tunnel_->waitForChild();
        output_->verbose(CALL_INFO, 1, 0, "Plugin attached!\n");
        stop_ticking_ = false;
    }

    for (uint32_t i = 0; i < cfg_.vcpu_count; i++)
        mem_ifaces_[i]->init(phase);
}

void QuetzCPU::finish() {
    output_->verbose(CALL_INFO, 1, 0,
        "QuetzComponent finishing at %" PRIu64 " ns.\n",
        (uint64_t)getCurrentSimTimeNano());

    for (uint32_t i = 0; i < cfg_.vcpu_count; i++)
        cores_[i]->finishCore();

    launcher_.terminate();
}

void QuetzCPU::emergencyShutdown() {
    launcher_.forceKill();
    delete tunnelmgr_;
    tunnelmgr_ = nullptr;
}

bool QuetzCPU::tick(SST::Cycle_t /*cycle*/) {
    tunnel_->updateTime(getCurrentSimTimeNano());
    tunnel_->incrementCycles();

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
