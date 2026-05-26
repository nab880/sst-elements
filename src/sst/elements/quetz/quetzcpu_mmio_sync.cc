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
        mmio_sync_state_->pending[req->getID()] = { vcpu, (uint8_t)cmd.size, false };
        iface->send(req);
        cores_[vcpu]->recordMmioSyncRequest(false);
        return true;
    }

    return false;
}

bool QuetzCPU::completeMmioSyncResponse(uint32_t vcpu_hint,
                                        StandardMem::Request* resp)
{
    if (!mmio_sync_state_)
        return false;

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
