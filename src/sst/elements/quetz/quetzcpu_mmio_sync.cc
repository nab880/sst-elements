// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include <sst_config.h>
#include "quetzcpu.h"

#include "quetz_balar_accelerator_port.h"

#include <inttypes.h>
#include <vector>

using namespace SST;
using namespace SST::Interfaces;
using namespace SST::Quetz;

// ---------------------------------------------------------------------------
// Accelerator port wiring. QuetzCPU owns the generic synchronous-MMIO mailbox
// (post a request, forward a plain read/write to the mmio link, return the
// response). Doorbell coherence-flush, posted/async completion, and any
// accelerator-specific policy live in AcceleratorPort subcomponent(s); QuetzCPU
// just routes commands/responses to them and ticks them (P5 AcceleratorPort).
// ---------------------------------------------------------------------------

void QuetzCPU::loadAcceleratorPorts()
{
    accel_ports_.clear();

    SubComponentSlotInfo* slot = getSubComponentSlotInfo("accelerator");
    if (slot && slot->isPopulated(0)) {
        for (int i = 0; i <= slot->getMaxPopulatedSlotNumber(); i++) {
            if (!slot->isPopulated(i))
                continue;
            auto* p = slot->create<AcceleratorPort>(
                i, ComponentInfo::SHARE_NONE, (AcceleratorHost*)this);
            if (p)
                accel_ports_.push_back(p);
        }
        output_->verbose(CALL_INFO, 1, 0,
            "Loaded %zu accelerator port(s) from the 'accelerator' slot.\n",
            accel_ports_.size());
        return;
    }

    // Back-compat: no explicit slot, but a balar doorbell is configured — create
    // the default balar port from the legacy cpu params so existing SDLs work
    // unchanged.
    if (cfg_.balar_doorbell_addr != 0) {
        Params p;
        p.insert("verbose", std::to_string(cfg_.verbosity));
        p.insert("doorbell_addr", std::to_string(cfg_.balar_doorbell_addr));
        p.insert("doorbell_size", std::to_string(cfg_.balar_doorbell_size));
        p.insert("packet_flush_bytes", std::to_string(cfg_.balar_packet_flush_bytes));
        p.insert("async_offload", cfg_.async_offload ? "1" : "0");
        p.insert("async_doorbell_addr", std::to_string(cfg_.async_doorbell_addr));
        p.insert("async_doorbell_size", std::to_string(cfg_.async_doorbell_size));
        p.insert("async_completion_depth",
                 std::to_string(cfg_.async_completion_depth));
        auto* port = loadAnonymousSubComponent<AcceleratorPort>(
            "quetz.BalarAcceleratorPort", "accelerator", 0,
            ComponentInfo::SHARE_NONE, p, (AcceleratorHost*)this);
        if (port)
            accel_ports_.push_back(port);
        output_->verbose(CALL_INFO, 1, 0,
            "Auto-created a default BalarAcceleratorPort (doorbell=0x%016" PRIx64
            ", async=%d).\n", cfg_.balar_doorbell_addr, (int)cfg_.async_offload);
    }
}

AcceleratorPort* QuetzCPU::portForAddr(uint64_t addr) const
{
    for (auto* p : accel_ports_)
        if (p->ownsAddr(addr))
            return p;
    return nullptr;
}

void QuetzCPU::pollMmioSyncMailbox()
{
    QuetzSharedData* shared = frontend_->tunnel()->getSharedData();
    if (!shared)
        return;

    for (uint32_t v = 0; v < cfg_.vcpu_count; v++) {
        QuetzMmioSyncRequest* req = &shared->mmio_req[v];
        // Acquire pairs with the producer's (QEMU bridge) release-store of
        // `pending`, so the request fields it published beforehand are visible
        // here even on weak-memory (e.g. ARM) hosts.
        if (__atomic_load_n(&req->pending, __ATOMIC_ACQUIRE) == 0)
            continue;

        uint32_t cmd = req->cmd;
        uint64_t addr = req->addr;
        uint32_t size = req->size;
        uint64_t wval = req->write_val;
        // Release so the producer sees our field reads as complete before it can
        // reuse the slot for the next request.
        __atomic_store_n(&req->pending, 0u, __ATOMIC_RELEASE);

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

    // Accelerator apertures (doorbell / async submit) are handled by their port.
    if (AcceleratorPort* port = portForAddr(cmd.addr)) {
        port->handleCommand(vcpu, cmd);
        return true;
    }

    // Generic synchronous MMIO: forward the plain read/write to the mmio link
    // and return its response from completeMmioSyncResponse.
    StandardMem* iface = mmio_ifaces_[vcpu];

    if (cmd.cmd == QUETZ_CMD_MMIO_READ_REQ) {
        auto* req = new StandardMem::Read(cmd.addr, cmd.size);
        generic_pending_[req->getID()] = { vcpu, true };
        iface->send(req);
        cores_[vcpu]->recordMmioSyncRequest(true);
        return true;
    }

    if (cmd.cmd == QUETZ_CMD_MMIO_WRITE_REQ) {
        std::vector<uint8_t> payload(cmd.data, cmd.data + cmd.size);
        auto* req = new StandardMem::Write(cmd.addr, cmd.size, payload);
        generic_pending_[req->getID()] = { vcpu, false };
        iface->send(req);
        cores_[vcpu]->recordMmioSyncRequest(false);
        return true;
    }

    return false;
}

bool QuetzCPU::completeMmioSyncResponse(uint32_t vcpu_hint,
                                        StandardMem::Request* resp)
{
    // Offer the response to each accelerator port first (flushes / forwarded
    // doorbells); a port that owns it consumes (and deletes) it.
    for (auto* port : accel_ports_) {
        if (port->handleResponse(vcpu_hint, resp))
            return true;
    }

    auto it = generic_pending_.find(resp->getID());
    if (it == generic_pending_.end())
        return false;

    uint32_t vcpu = it->second.vcpu;
    uint64_t value = 0;
    if (it->second.is_read) {
        auto* rresp = dynamic_cast<StandardMem::ReadResp*>(resp);
        if (rresp)
            value = accelDataToU64(rresp->data);
    }
    generic_pending_.erase(it);
    delete resp;

    frontend_->tunnel()->mmioSync().postResponse(vcpu, value);
    return true;
}

bool QuetzCPU::hasAsyncInFlight() const
{
    for (auto* p : accel_ports_)
        if (p->hasOutstanding())
            return true;
    return false;
}

bool QuetzCPU::asyncOutstandingForVcpu(uint32_t vcpu) const
{
    for (auto* p : accel_ports_)
        if (p->vcpuHasOutstanding(vcpu))
            return true;
    return false;
}

// --- AcceleratorHost services ---------------------------------------------

void QuetzCPU::postResponse(uint32_t vcpu, uint64_t value)
{
    frontend_->tunnel()->mmioSync().postResponse(vcpu, value);
}

void QuetzCPU::sendMem(uint32_t vcpu, StandardMem::Request* r)
{
    mem_ifaces_[vcpu]->send(r);
}

void QuetzCPU::sendMmio(uint32_t vcpu, StandardMem::Request* r)
{
    mmio_ifaces_[vcpu]->send(r);
}

bool QuetzCPU::isDrained(uint32_t vcpu) const { return cores_[vcpu]->isDrained(); }
bool QuetzCPU::hasMem(uint32_t vcpu)  const { return mem_ifaces_[vcpu]  != nullptr; }
bool QuetzCPU::hasMmio(uint32_t vcpu) const { return mmio_ifaces_[vcpu] != nullptr; }
uint64_t QuetzCPU::cacheLineSize() const { return cfg_.cache_line_sz; }
uint64_t QuetzCPU::cycles() const { return frontend_->tunnel()->getCycles(); }

void QuetzCPU::recordSyncRequest(uint32_t vcpu, bool is_read) {
    cores_[vcpu]->recordMmioSyncRequest(is_read);
}
void QuetzCPU::recordDoorbellFlush(uint32_t vcpu) {
    cores_[vcpu]->recordMmioDoorbellFlush();
}
void QuetzCPU::recordDoorbellFlushCycles(uint32_t vcpu, uint64_t c) {
    cores_[vcpu]->recordMmioDoorbellFlushCycles(c);
}
void QuetzCPU::recordAsyncSubmit(uint32_t vcpu) {
    cores_[vcpu]->recordAsyncSubmit();
}
void QuetzCPU::recordAsyncCompletion(uint32_t vcpu) {
    cores_[vcpu]->recordAsyncCompletion();
}
