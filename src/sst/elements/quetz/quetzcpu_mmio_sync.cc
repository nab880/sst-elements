// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include <sst_config.h>
#include "quetzcpu.h"

#include <inttypes.h>
#include <unordered_map>
#include <vector>

using namespace SST;
using namespace SST::Interfaces;
using namespace SST::Quetz;

namespace {

struct MmioSyncPending {
    uint32_t vcpu;
    uint8_t  size;
    bool     is_read;
};

struct DoorbellFlushPending {
    uint32_t vcpu;
    uint32_t remaining;
    uint64_t start_cycle;
    StandardMem::Write* doorbell;
};

static uint64_t dataToU64(const std::vector<uint8_t>& data)
{
    uint64_t val = 0;
    for (int i = (int)data.size() - 1; i >= 0; i--) {
        val <<= 8;
        val |= data[(size_t)i];
    }
    return val;
}

static std::vector<uint8_t> u64ToData(uint64_t val, size_t size)
{
    std::vector<uint8_t> out(size, 0);
    for (size_t i = 0; i < size; i++) {
        out[i] = (uint8_t)(val & 0xFF);
        val >>= 8;
    }
    return out;
}

} // namespace

struct QuetzCPU::MmioSyncState {
    std::unordered_map<uint64_t, MmioSyncPending> pending;
    std::unordered_map<uint64_t, uint32_t> flush_to_vcpu;
    std::unordered_map<uint32_t, DoorbellFlushPending> doorbell_flushes;
};

void QuetzCPU::initMmioSyncState()
{
    if (!mmio_sync_state_)
        mmio_sync_state_ = new MmioSyncState();
}

void QuetzCPU::pollMmioSyncMailbox()
{
    QuetzSharedData* shared = frontend_->tunnel()->getSharedData();
    if (!shared)
        return;

    initMmioSyncState();

    for (uint32_t v = 0; v < cfg_.vcpu_count; v++) {
        QuetzMmioSyncRequest* req = &shared->mmio_req[v];
        if (req->pending == 0)
            continue;

        uint32_t cmd = req->cmd;
        uint64_t addr = req->addr;
        uint32_t size = req->size;
        uint64_t wval = req->write_val;
        req->pending = 0;
        __sync_synchronize();

        QuetzCommand fake{};
        fake.cmd  = (QuetzShmemCmd)cmd;
        fake.addr = addr;
        fake.size = size;
        if (cmd == QUETZ_CMD_MMIO_WRITE_REQ) {
            for (uint32_t i = 0; i < size && i < sizeof(fake.data); i++)
                fake.data[i] = (uint8_t)((wval >> (8 * i)) & 0xFF);
        }
        handleMmioSyncCommand(v, fake);
    }
}

bool QuetzCPU::handleMmioSyncCommand(uint32_t vcpu, const QuetzCommand& cmd)
{
    if (!mmio_ifaces_[vcpu]) {
        output_->verbose(CALL_INFO, 1, 0,
            "vCPU %" PRIu32 ": MMIO sync cmd but no mmio_link — dropping\n", vcpu);
        frontend_->tunnel()->mmioSync().postResponse(vcpu, 0);
        return true;
    }

    initMmioSyncState();
    StandardMem* iface = mmio_ifaces_[vcpu];

    if (cmd.cmd == QUETZ_CMD_MMIO_READ_REQ) {
        auto* req = new StandardMem::Read(cmd.addr, cmd.size);
        mmio_sync_state_->pending[req->getID()] = { vcpu, (uint8_t)cmd.size, true };
        iface->send(req);
        cores_[vcpu]->recordMmioSyncRequest(true);
        return true;
    }

    if (cmd.cmd == QUETZ_CMD_MMIO_WRITE_REQ) {
        std::vector<uint8_t> payload(cmd.data, cmd.data + cmd.size);
        auto* req = new StandardMem::Write(cmd.addr, cmd.size, payload);
        const uint64_t doorbell_start = cfg_.balar_doorbell_addr;
        const uint64_t doorbell_end = doorbell_start + cfg_.balar_doorbell_size;
        const bool is_balar_doorbell =
            doorbell_start != 0 &&
            cfg_.balar_doorbell_size != 0 &&
            cmd.addr >= doorbell_start && cmd.addr < doorbell_end &&
            mem_ifaces_[vcpu] != nullptr;

        if (!is_balar_doorbell || cfg_.balar_packet_flush_bytes == 0) {
            forwardMmioSyncWrite(vcpu, req);
            return true;
        }

        if (mmio_sync_state_->doorbell_flushes.count(vcpu) != 0) {
            output_->fatal(CALL_INFO, -1,
                "vCPU %" PRIu32 ": nested balar doorbell while a flush is pending.\n",
                vcpu);
        }

        const uint64_t scratch = dataToU64(payload);
        const uint64_t line_size = cfg_.cache_line_sz;
        const uint64_t first_line = (scratch / line_size) * line_size;
        const uint64_t end = scratch + cfg_.balar_packet_flush_bytes;

        DoorbellFlushPending ctx{};
        ctx.vcpu = vcpu;
        ctx.remaining = 0;
        ctx.start_cycle = frontend_->tunnel()->getCycles();
        ctx.doorbell = req;

        for (uint64_t line = first_line; line < end; line += line_size) {
            auto* flush = new StandardMem::FlushAddr(line, line_size, true, 1);
            mmio_sync_state_->flush_to_vcpu[flush->getID()] = vcpu;
            ctx.remaining++;
            cores_[vcpu]->recordMmioDoorbellFlush();
            mem_ifaces_[vcpu]->send(flush);
        }

        if (ctx.remaining == 0) {
            forwardMmioSyncWrite(vcpu, req);
            return true;
        }

        mmio_sync_state_->doorbell_flushes[vcpu] = ctx;
        return true;
    }

    return false;
}

void QuetzCPU::forwardMmioSyncWrite(uint32_t vcpu, StandardMem::Write* req)
{
    mmio_sync_state_->pending[req->getID()] = { vcpu, (uint8_t)req->size, false };
    mmio_ifaces_[vcpu]->send(req);
    cores_[vcpu]->recordMmioSyncRequest(false);
}

bool QuetzCPU::completeMmioSyncResponse(uint32_t vcpu_hint,
                                        StandardMem::Request* resp)
{
    if (!mmio_sync_state_)
        return false;

    auto fit = mmio_sync_state_->flush_to_vcpu.find(resp->getID());
    if (fit != mmio_sync_state_->flush_to_vcpu.end()) {
        uint32_t vcpu = fit->second;
        mmio_sync_state_->flush_to_vcpu.erase(fit);

        auto dit = mmio_sync_state_->doorbell_flushes.find(vcpu);
        if (dit == mmio_sync_state_->doorbell_flushes.end()) {
            output_->fatal(CALL_INFO, -1,
                "vCPU %" PRIu32 ": balar doorbell flush response without context.\n",
                vcpu);
        }

        if (!dynamic_cast<StandardMem::FlushResp*>(resp)) {
            output_->fatal(CALL_INFO, -1,
                "vCPU %" PRIu32 ": balar doorbell flush completed with non-FlushResp.\n",
                vcpu);
        }

        if (dit->second.remaining > 0)
            dit->second.remaining--;

        delete resp;

        if (dit->second.remaining == 0) {
            const uint64_t elapsed =
                frontend_->tunnel()->getCycles() - dit->second.start_cycle;
            StandardMem::Write* doorbell = dit->second.doorbell;
            cores_[vcpu]->recordMmioDoorbellFlushCycles(elapsed);
            mmio_sync_state_->doorbell_flushes.erase(dit);
            forwardMmioSyncWrite(vcpu, doorbell);
        }
        return true;
    }

    auto it = mmio_sync_state_->pending.find(resp->getID());
    if (it == mmio_sync_state_->pending.end())
        return false;

    uint32_t vcpu = it->second.vcpu;
    (void)vcpu_hint;

    uint64_t value = 0;
    if (it->second.is_read) {
        auto* rresp = dynamic_cast<StandardMem::ReadResp*>(resp);
        if (rresp)
            value = dataToU64(rresp->data);
    }

    mmio_sync_state_->pending.erase(it);
    delete resp;

    frontend_->tunnel()->mmioSync().postResponse(vcpu, value);
    return true;
}
