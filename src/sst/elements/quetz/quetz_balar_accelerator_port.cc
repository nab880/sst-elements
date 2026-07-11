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
#include "quetz_balar_accelerator_port.h"
#include "quetz_balar_flush_range.h"

#include <inttypes.h>
#include <limits>

using namespace SST;
using namespace SST::Interfaces;
using namespace SST::Quetz;

BalarAcceleratorPort::BalarAcceleratorPort(ComponentId_t id, Params& params,
                                           AcceleratorHost* host)
    : AcceleratorPort(id, params),
      host_(host)
{
    out_.init("BalarAcceleratorPort[@p:@l]: ",
              params.find<int>("verbose", 0), 0, Output::STDOUT);

    doorbell_addr_          = params.find<uint64_t>("doorbell_addr", 0);
    doorbell_size_          = params.find<uint64_t>("doorbell_size", 8);
    packet_flush_bytes_     = params.find<uint64_t>("packet_flush_bytes", 4096);

    std::string flush_mode  = params.find<std::string>("flush_mode", "range_from_value");
    if (flush_mode == "none") {
        flush_enabled_ = false;
    } else if (flush_mode == "range_from_value") {
        flush_enabled_ = true;
    } else {
        out_.fatal(CALL_INFO, -1,
            "unknown flush_mode '%s' (expected 'range_from_value' or 'none').\n",
            flush_mode.c_str());
    }

    async_offload_          = params.find<bool>("async_offload", false);
    async_doorbell_addr_    = params.find<uint64_t>("async_doorbell_addr", 0);
    async_doorbell_size_    = params.find<uint64_t>("async_doorbell_size", 0x40);
    async_completion_depth_ = params.find<uint32_t>("async_completion_depth", 1);
    if (async_completion_depth_ == 0)
        async_completion_depth_ = 1;
}

bool BalarAcceleratorPort::ownsAddr(uint64_t addr) const
{
    const bool in_doorbell =
        doorbell_addr_ != 0 && doorbell_size_ != 0 &&
        addr >= doorbell_addr_ && addr < doorbell_addr_ + doorbell_size_;
    const bool in_async =
        async_offload_ && async_doorbell_addr_ != 0 &&
        addr >= async_doorbell_addr_ &&
        addr < async_doorbell_addr_ + async_doorbell_size_;
    return in_doorbell || in_async;
}

void BalarAcceleratorPort::handleCommand(uint32_t vcpu, const QuetzCommand& cmd)
{
    // The async submit/completion aperture is emulated entirely here (balar
    // exposes no completion-queue ABI): reads are answered from local state, and
    // a SUBMIT posts the balar doorbell without blocking the guest for the kernel.
    if (async_offload_ && async_doorbell_addr_ != 0 &&
        cmd.addr >= async_doorbell_addr_ &&
        cmd.addr <  async_doorbell_addr_ + async_doorbell_size_) {
        handleAsyncAperture(vcpu, cmd);
        return;
    }

    // Doorbell aperture. Reads (e.g. the cb_ring return value or D2H copy-back)
    // forward generically; writes are the doorbell itself.
    if (cmd.cmd == QUETZ_CMD_MMIO_READ_REQ) {
        auto* req = new StandardMem::Read(cmd.addr, cmd.size);
        pending_[req->getID()] = { vcpu, true, false };
        host_->sendMmio(vcpu, req);
        host_->recordSyncRequest(vcpu, true);
        return;
    }

    if (cmd.cmd == QUETZ_CMD_MMIO_WRITE_REQ) {
        std::vector<uint8_t> payload(cmd.data, cmd.data + cmd.size);
        auto* req = new StandardMem::Write(cmd.addr, cmd.size, payload);
        const bool can_flush = host_->hasMem(vcpu);

        out_.verbose(CALL_INFO, 1, 0,
            "vCPU %" PRIu32 ": doorbell write addr=0x%016" PRIx64
            " size=%" PRIu32 " mem_iface=%s flush_bytes=%" PRIu64 "\n",
            vcpu, cmd.addr, cmd.size, can_flush ? "yes" : "no",
            packet_flush_bytes_);

        if (!can_flush || !flush_enabled_ || packet_flush_bytes_ == 0) {
            forwardDoorbell(vcpu, req, false, false);
            return;
        }

        // Arm-and-defer: the guest stays blocked on this synchronous doorbell
        // while its packet writes (still in flight through the rate-limited
        // pipeline) drain into SST memory. process() issues the flushes once the
        // vCPU is drained — flushing now could push a partial packet to balar.
        if (armed_doorbells_.count(vcpu) != 0) {
            out_.fatal(CALL_INFO, -1,
                "vCPU %" PRIu32 ": nested doorbell while one is armed.\n", vcpu);
        }
        armed_doorbells_[vcpu] = { req, false, false };
        return;
    }
}

void BalarAcceleratorPort::handleAsyncAperture(uint32_t vcpu,
                                               const QuetzCommand& cmd)
{
    // The async engine's in-flight/depth state is global (not per-vCPU), so it
    // only supports a single vCPU today. Fail loud if a second one touches it.
    if (async_vcpu_ < 0) {
        async_vcpu_ = (int)vcpu;
    } else if ((int)vcpu != async_vcpu_) {
        out_.fatal(CALL_INFO, -1,
            "async offload is single-vCPU only (global in-flight/depth state); "
            "vCPU %" PRIu32 " used it after vCPU %d.\n", vcpu, async_vcpu_);
    }

    const uint64_t off = cmd.addr - async_doorbell_addr_;

    if (cmd.cmd == QUETZ_CMD_MMIO_READ_REQ) {
        uint64_t value = 0;
        switch (off) {
            case ASYNC_REG_STATUS:    value = async_in_flight_;     break;
            case ASYNC_REG_COMPLETED: value = completed_id_[vcpu];  break;
            case ASYNC_REG_TICKET:    value = submit_id_[vcpu];     break;
            case ASYNC_REG_RESULT:    value = last_result_[vcpu];   break;
            default:                  value = 0;                    break;
        }
        host_->postResponse(vcpu, value);
        host_->recordSyncRequest(vcpu, true);
        return;
    }

    if (cmd.cmd == QUETZ_CMD_MMIO_WRITE_REQ) {
        if (off != ASYNC_REG_SUBMIT) {
            // Writes to the read-only async registers are ignored but acked.
            host_->postResponse(vcpu, 0);
            return;
        }
        if (doorbell_addr_ == 0 || !host_->hasMem(vcpu)) {
            out_.fatal(CALL_INFO, -1,
                "vCPU %" PRIu32 ": async SUBMIT requires doorbell_addr and a "
                "cache (mem) interface.\n", vcpu);
        }

        // The submitted value is the guest scratch-packet address; forward it as
        // a balar doorbell write.
        uint64_t scratch = 0;
        for (uint32_t i = 0; i < cmd.size && i < sizeof(cmd.data); i++)
            scratch |= (uint64_t)cmd.data[i] << (8 * i);

        // Bound outstanding posted ops to async_completion_depth.
        if (async_in_flight_ + 1 > async_completion_depth_) {
            out_.fatal(CALL_INFO, -1,
                "vCPU %" PRIu32 ": async SUBMIT exceeds async_completion_depth=%"
                PRIu32 " (%" PRIu32 " already in flight). The guest must poll "
                "STATUS/COMPLETED before submitting more.\n",
                vcpu, async_completion_depth_, async_in_flight_);
        }

        submit_id_[vcpu]++;
        async_in_flight_++;
        host_->recordAsyncSubmit(vcpu);

        // Forward the balar doorbell at the guest's submit width (4B on ColdFire,
        // 8B on RISC-V) so it is byte-identical to the synchronous doorbell write.
        uint32_t db_size = cmd.size ? cmd.size : (uint32_t)doorbell_size_;
        auto* doorbell = new StandardMem::Write(
            doorbell_addr_, db_size, accelU64ToData(scratch, db_size));

        if (!async_busy_[vcpu]) {
            // Head of the queue: arm-and-defer exactly like the synchronous
            // doorbell. The guest stays blocked on this MMIO write through the
            // (short) drain+flush+forward — so the kernel is running in balar
            // before the guest is acked and proceeds, giving real overlap.
            async_busy_[vcpu] = true;
            armed_doorbells_[vcpu] = { doorbell, true, false };
        } else {
            // Another posted op is in process; balar serves one blocking op at a
            // time. Queue this one and ack the guest now — it was staged before
            // SUBMIT, so the deferred flush (issued when this op reaches the head)
            // still captures committed bytes. Buffer-lifetime rule: the guest
            // must keep this op's packet alive until it completes.
            submit_queue_[vcpu].push_back(doorbell);
            host_->postResponse(vcpu, 0);
        }
        out_.verbose(CALL_INFO, 1, 0,
            "vCPU %" PRIu32 ": async SUBMIT ticket=%" PRIu64
            " scratch=0x%016" PRIx64 " in_flight=%" PRIu32 " queued=%zu\n",
            vcpu, submit_id_[vcpu], scratch, async_in_flight_,
            submit_queue_[vcpu].size());
        return;
    }
}

void BalarAcceleratorPort::process()
{
    if (armed_doorbells_.empty())
        return;

    for (auto it = armed_doorbells_.begin(); it != armed_doorbells_.end(); ) {
        uint32_t vcpu = it->first;
        ArmedDoorbell armed = it->second;
        if (host_->isDrained(vcpu)) {
            it = armed_doorbells_.erase(it);
            issueDoorbellFlushes(vcpu, armed.req, armed.is_async, armed.pre_acked);
        } else {
            ++it;
        }
    }
}

void BalarAcceleratorPort::issueDoorbellFlushes(uint32_t vcpu,
                                                StandardMem::Write* req,
                                                bool is_async, bool pre_acked)
{
    if (doorbell_flushes_.count(vcpu) != 0) {
        out_.fatal(CALL_INFO, -1,
            "vCPU %" PRIu32 ": nested doorbell while a flush is pending.\n", vcpu);
    }

    const uint64_t flush_bytes = flush_enabled_ ? packet_flush_bytes_ : 0;
    const uint64_t scratch = accelDataToU64(req->data);
    const uint64_t line_size = host_->cacheLineSize();
    BalarFlushRange range;
    if (!computeBalarFlushRange(scratch, flush_bytes, line_size, range)) {
        out_.fatal(CALL_INFO, -1,
            "vCPU %" PRIu32 ": invalid doorbell flush range: scratch=0x%016"
            PRIx64 " flush_bytes=%" PRIu64 " line_size=%" PRIu64
            " (address arithmetic overflow or zero line size).\n",
            vcpu, scratch, flush_bytes, line_size);
    }
    if (range.line_count > std::numeric_limits<uint32_t>::max()) {
        out_.fatal(CALL_INFO, -1,
            "vCPU %" PRIu32 ": doorbell flush range requires %" PRIu64
            " lines, exceeding the supported maximum of %" PRIu32 ".\n",
            vcpu, range.line_count,
            std::numeric_limits<uint32_t>::max());
    }

    out_.verbose(CALL_INFO, 1, 0,
        "vCPU %" PRIu32 ": doorbell scratch=0x%016" PRIx64
        " flush_range=[0x%016" PRIx64 ",0x%016" PRIx64 ") line_size=%" PRIu64 "\n",
        vcpu, scratch, range.first_line, range.end, line_size);

    FlushCtx ctx{};
    ctx.vcpu = vcpu;
    ctx.remaining = 0;
    ctx.start_cycle = host_->cycles();
    ctx.doorbell = req;
    ctx.is_async = is_async;
    ctx.pre_acked = pre_acked;

    uint64_t line = range.first_line;
    for (uint64_t i = 0; i < range.line_count; ++i) {
        auto* flush = new StandardMem::FlushAddr(line, line_size, true, 1);
        flush_to_vcpu_[flush->getID()] = vcpu;
        ctx.remaining++;
        host_->recordDoorbellFlush(vcpu);
        host_->sendMem(vcpu, flush);

        if (i + 1 < range.line_count) {
            if (line > std::numeric_limits<uint64_t>::max() - line_size) {
                out_.fatal(CALL_INFO, -1,
                    "vCPU %" PRIu32 ": doorbell flush line progression "
                    "overflows after address 0x%016" PRIx64 ".\n",
                    vcpu, line);
            }
            line += line_size;
        }
    }

    if (ctx.remaining == 0) {
        forwardDoorbell(vcpu, req, is_async, pre_acked);
        return;
    }

    out_.verbose(CALL_INFO, 1, 0,
        "vCPU %" PRIu32 ": waiting for %" PRIu32
        " doorbell flush responses before forwarding doorbell\n",
        vcpu, ctx.remaining);
    doorbell_flushes_[vcpu] = ctx;
}

void BalarAcceleratorPort::forwardDoorbell(uint32_t vcpu,
                                           StandardMem::Write* req,
                                           bool is_async, bool pre_acked)
{
    pending_[req->getID()] = { vcpu, false, is_async };
    host_->sendMmio(vcpu, req);
    host_->recordSyncRequest(vcpu, false);
    if (is_async && !pre_acked) {
        // Posted offload (head of queue): the doorbell has reached balar, so ack
        // the guest's SUBMIT now. balar holds its (deferred) write response until
        // the op finishes; that response advances the COMPLETED counter the guest
        // polls (handleResponse). Queued ops (pre_acked) were already acked.
        host_->postResponse(vcpu, 0);
    }
}

bool BalarAcceleratorPort::handleResponse(uint32_t vcpu_hint,
                                          StandardMem::Request* resp)
{
    (void)vcpu_hint;

    auto fit = flush_to_vcpu_.find(resp->getID());
    if (fit != flush_to_vcpu_.end()) {
        uint32_t vcpu = fit->second;
        flush_to_vcpu_.erase(fit);

        auto dit = doorbell_flushes_.find(vcpu);
        if (dit == doorbell_flushes_.end()) {
            out_.fatal(CALL_INFO, -1,
                "vCPU %" PRIu32 ": doorbell flush response without context.\n",
                vcpu);
        }
        if (!dynamic_cast<StandardMem::FlushResp*>(resp)) {
            out_.fatal(CALL_INFO, -1,
                "vCPU %" PRIu32 ": doorbell flush completed with non-FlushResp.\n",
                vcpu);
        }

        if (dit->second.remaining > 0)
            dit->second.remaining--;

        out_.verbose(CALL_INFO, 4, 0,
            "vCPU %" PRIu32 ": doorbell flush response received, remaining=%"
            PRIu32 "\n", vcpu, dit->second.remaining);

        delete resp;

        if (dit->second.remaining == 0) {
            const uint64_t elapsed = host_->cycles() - dit->second.start_cycle;
            StandardMem::Write* doorbell = dit->second.doorbell;
            bool is_async = dit->second.is_async;
            bool pre_acked = dit->second.pre_acked;
            host_->recordDoorbellFlushCycles(vcpu, elapsed);
            doorbell_flushes_.erase(dit);
            out_.verbose(CALL_INFO, 1, 0,
                "vCPU %" PRIu32 ": all doorbell flush responses complete, "
                "forwarding doorbell after %" PRIu64 " cycles\n", vcpu, elapsed);
            forwardDoorbell(vcpu, doorbell, is_async, pre_acked);
        }
        return true;
    }

    auto it = pending_.find(resp->getID());
    if (it == pending_.end())
        return false;

    uint32_t vcpu = it->second.vcpu;
    bool is_read = it->second.is_read;
    bool is_async = it->second.is_async;

    uint64_t value = 0;
    if (is_read) {
        auto* rresp = dynamic_cast<StandardMem::ReadResp*>(resp);
        if (rresp)
            value = accelDataToU64(rresp->data);
    }

    pending_.erase(it);
    delete resp;

    if (is_async) {
        // Posted balar offload retired (balar released its deferred write
        // response): advance the completion counter the guest polls and drop the
        // sim hold. The guest was already acked at submit time.
        completed_id_[vcpu]++;
        last_result_[vcpu] = value;
        if (async_in_flight_ > 0)
            async_in_flight_--;
        async_busy_[vcpu] = false;
        host_->recordAsyncCompletion(vcpu);
        out_.verbose(CALL_INFO, 1, 0,
            "vCPU %" PRIu32 ": async offload completed, completed_id=%" PRIu64
            " in_flight=%" PRIu32 "\n", vcpu, completed_id_[vcpu], async_in_flight_);

        // Start the next queued op (FIFO). It was acked at submit; arm-and-defer
        // it now — its packet committed before its submit, so the deferred flush
        // (once the vCPU next drains) captures the right bytes.
        auto qit = submit_queue_.find(vcpu);
        if (qit != submit_queue_.end() && !qit->second.empty()) {
            StandardMem::Write* next = qit->second.front();
            qit->second.pop_front();
            async_busy_[vcpu] = true;
            armed_doorbells_[vcpu] = { next, true, true };
        }
    } else {
        host_->postResponse(vcpu, value);
    }
    return true;
}

bool BalarAcceleratorPort::hasOutstanding() const
{
    return async_in_flight_ > 0;
}

bool BalarAcceleratorPort::vcpuHasOutstanding(uint32_t vcpu) const
{
    auto s = submit_id_.find(vcpu);
    if (s == submit_id_.end())
        return false;
    uint64_t completed = 0;
    auto c = completed_id_.find(vcpu);
    if (c != completed_id_.end())
        completed = c->second;
    return s->second > completed;
}
