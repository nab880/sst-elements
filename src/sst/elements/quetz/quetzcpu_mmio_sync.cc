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

bool QuetzCPU::windowBigEndian(uint64_t addr) const
{
    if (cfg_.sst_window_cache)
        return cfg_.window_big_endian && window_caches_.cache(0).contains(addr);
    return cfg_.window_big_endian && cfg_.sst_window_size != 0 &&
           addr >= cfg_.sst_window_base &&
           addr < cfg_.sst_window_base + cfg_.sst_window_size;
}

void QuetzCPU::pollMmioSyncMailbox()
{
    QuetzSharedData* shared = frontend_->tunnel()->getSharedData();
    if (!shared)
        return;

    for (uint32_t v = 0; v < cfg_.vcpu_count; v++) {
        if (cfg_.sst_window_cache) {
            const auto epoch = __atomic_load_n(&shared->cpu_reset_epoch[v], __ATOMIC_ACQUIRE);
            if (epoch != cache_reset_epochs_[v]) {
                try {
                    window_caches_.resetCore(v);
                    cache_reset_epochs_[v] = epoch;
                } catch (const std::exception& e) {
                    output_->fatal(CALL_INFO, -1, "Window cache reset vCPU %u: %s\n", v, e.what());
                }
            }
        }
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
        if (cmd == QUETZ_CMD_CACHE_OP) {
            // Cache op operand is a numeric uint32 independent of aperture byte order.
            for (unsigned i = 0; i < 4; ++i) fake.data[i] = wval >> (8 * i);
        }
        if (cmd == QUETZ_CMD_MMIO_WRITE_REQ) {
            // The mailbox carries the numeric value; serialize it into memory
            // byte order. LSB-first is the default; SST-window accesses pack
            // MSB-first when window_big_endian is set so the stored bytes
            // match a big-endian guest's real memory layout. (size > 8 never
            // comes from the bridge — max_access_size is 8 — but guard the
            // shift anyway.)
            bool be = windowBigEndian(addr) && size <= 8;
            for (uint32_t i = 0; i < size && i < sizeof(fake.data); i++) {
                uint32_t shift = be ? (size - 1 - i) : i;
                fake.data[i] = (uint8_t)((wval >> (8 * shift)) & 0xFF);
            }
        }
        handleMmioSyncCommand(v, fake);
    }
}

bool QuetzCPU::handleMmioSyncCommand(uint32_t vcpu, const QuetzCommand& cmd)
{
    if (vcpu >= cfg_.vcpu_count)
        output_->fatal(CALL_INFO, -1, "MMIO command has invalid vCPU %u.\n", vcpu);
    if (cmd.cmd == QUETZ_CMD_CACHE_OP ||
        (cfg_.sst_window_cache && window_caches_.cache(vcpu).contains(cmd.addr))) {
        if (!cfg_.sst_window_cache || !mmio_ifaces_[vcpu])
            output_->fatal(CALL_INFO, -1, "Cache mailbox requires an enabled window cache and MMIO interface.\n");
        try {
            auto& cache = window_caches_.cache(vcpu);
            if (cmd.cmd == QUETZ_CMD_CACHE_OP) {
                uint32_t value = 0;
                for (unsigned i = 0; i < 4; ++i) value |= uint32_t(cmd.data[i]) << (8 * i);
                if (cmd.addr > UINT32_MAX) throw std::invalid_argument("invalid cache control operand");
                if (cmd.size == 0) cache.movec(cmd.addr, value);
                else if (cmd.size == 1) cache.push(value, cmd.addr);
                else throw std::invalid_argument("unknown cache maintenance command");
            } else if (cmd.cmd == QUETZ_CMD_MMIO_READ_REQ || cmd.cmd == QUETZ_CMD_MMIO_WRITE_REQ) {
                const bool write = cmd.cmd == QUETZ_CMD_MMIO_WRITE_REQ;
                if (cmd.size > sizeof(cmd.data)) throw std::invalid_argument("oversized cache access");
                cache.access(cmd.addr, cmd.size, write,
                    write ? std::vector<uint8_t>(cmd.data, cmd.data + cmd.size) : std::vector<uint8_t>{});
                cores_[vcpu]->recordMmioSyncRequest(!write);
            } else {
                throw std::invalid_argument("unexpected cached-window command");
            }
            serviceWindowCache(vcpu);
        } catch (const std::exception& e) {
            output_->fatal(CALL_INFO, -1, "Window cache vCPU %u: %s\n", vcpu, e.what());
        }
        return true;
    }
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
        generic_pending_[req->getID()] = { vcpu, true, windowBigEndian(cmd.addr) };
        iface->send(req);
        cores_[vcpu]->recordMmioSyncRequest(true);
        return true;
    }

    if (cmd.cmd == QUETZ_CMD_MMIO_WRITE_REQ) {
        std::vector<uint8_t> payload(cmd.data, cmd.data + cmd.size);
        auto* req = new StandardMem::Write(cmd.addr, cmd.size, payload);
        generic_pending_[req->getID()] = { vcpu, false, false };
        iface->send(req);
        cores_[vcpu]->recordMmioSyncRequest(false);
        return true;
    }

    return false;
}

bool QuetzCPU::completeMmioSyncResponse(uint32_t vcpu_hint,
                                        StandardMem::Request* resp)
{
    if (const auto* pending = window_caches_.pending(resp->getID())) {
        const uint32_t vcpu = pending->vcpu;
        auto* read = dynamic_cast<StandardMem::ReadResp*>(resp);
        auto* write = dynamic_cast<StandardMem::WriteResp*>(resp);
        if (vcpu_hint != vcpu || resp->getFail() || (pending->read ? !read : !write))
            output_->fatal(CALL_INFO, -1, "Invalid or failed window-cache memory response.\n");
        try {
            window_caches_.complete(resp->getID(), vcpu, read != nullptr,
                                    read ? read->data : std::vector<uint8_t>{});
            delete resp;
            serviceWindowCache(vcpu);
        } catch (const std::exception& e) {
            output_->fatal(CALL_INFO, -1, "Window cache vCPU %u: %s\n", vcpu, e.what());
        }
        return true;
    }
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
        if (rresp) {
            if (it->second.win_be) {
                // Big-endian window read: byte 0 is the MSB (mirror of the
                // MSB-first pack in pollMmioSyncMailbox).
                for (uint8_t b : rresp->data)
                    value = (value << 8) | b;
            } else {
                value = accelDataToU64(rresp->data);
            }
        }
    }
    generic_pending_.erase(it);
    delete resp;

    frontend_->tunnel()->mmioSync().postResponse(vcpu, value);
    return true;
}

void QuetzCPU::serviceWindowCache(uint32_t vcpu)
{
    auto& cache = window_caches_.cache(vcpu);
    for (;;) {
        const auto action = cache.next();
        if (action.kind == WindowDataCache::Kind::Done) {
            uint64_t value = 0;
            for (uint8_t byte : action.bytes) value = (value << 8) | byte;
            frontend_->tunnel()->mmioSync().postResponse(vcpu, value);
            return;
        }
        const bool read = action.kind == WindowDataCache::Kind::Read;
        const uint64_t native_bases[] = {0x80000000, 0x40000000};
        uint8_t* native = nullptr;
        for (unsigned bank = 0; bank < 2; ++bank) {
            if (action.address >= native_bases[bank] &&
                action.address - native_bases[bank] < QUETZ_LOCAL_RAM_BYTES) {
                const auto offset = action.address - native_bases[bank];
                if (action.size > QUETZ_LOCAL_RAM_BYTES - offset)
                    throw std::runtime_error("cache backing access crosses native RAM boundary");
                native = quetzLocalRam(frontend_->tunnel()->getSharedData(), bank) + offset;
                break;
            }
        }
        if (native) {
            if (read) cache.complete({native, native + action.size});
            else {
                if (action.bytes.size() != action.size)
                    throw std::runtime_error("invalid native cache writeback size");
                std::copy(action.bytes.begin(), action.bytes.end(), native);
                cache.complete();
            }
            continue;
        }
        StandardMem::Request* request;
        if (read) request = new StandardMem::Read(action.address, action.size);
        else request = new StandardMem::Write(action.address, action.size, action.bytes);
        window_caches_.issued(vcpu, request->getID(), read);
        mmio_ifaces_[vcpu]->send(request);
        return;
    }
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
