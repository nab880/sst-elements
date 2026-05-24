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

#include "quetz_mem_issue.h"

#include <algorithm>
#include <cstring>
#include <inttypes.h>
#include <vector>

using namespace SST;
using namespace SST::Quetz;
using namespace SST::Interfaces;

MemRequestEmitter::MemRequestEmitter(
        ComponentExtension* comp,
        SST::Output*      out,
        uint32_t          coreID,
        TimeConverter     tc,
        uint64_t          cacheLineSize,
        uint32_t          checkAddresses,
        QuetzCoreStats&   stats)
    : comp_(comp),
      mem_link_(nullptr),
      mmio_link_(nullptr),
      output_(out),
      core_id_(coreID),
      tc_(tc),
      cache_line_size_(cacheLineSize),
      check_addresses_(checkAddresses),
      stats_(stats),
      pending_count_(0)
{}

SST::Interfaces::StandardMem* MemRequestEmitter::linkFor(IssuePath path) const {
    return (path == IssuePath::MMIO) ? mmio_link_ : mem_link_;
}

uint32_t MemRequestEmitter::slotsNeeded(uint64_t vaddr, uint32_t size,
                                        IssuePath path) const {
    if (path == IssuePath::MMIO)
        return 1;
    if (size == 0) return 1;
    uint64_t first_line = vaddr / cache_line_size_;
    uint64_t last_line  = (vaddr + size - 1) / cache_line_size_;
    return (uint32_t)(last_line - first_line + 1);
}

void MemRequestEmitter::issueRead(uint64_t vaddr, uint32_t size, uint64_t /*pc*/,
                                  IssuePath path) {
    output_->verbose(CALL_INFO, 8, 0,
        "QuetzCore %" PRIu32 " READ  vaddr=0x%016" PRIx64 " size=%" PRIu32
        " path=%s\n",
        core_id_, vaddr, size,
        (path == IssuePath::MMIO) ? "mmio" : "cached");

    if (path != IssuePath::MMIO)
        stats_.read_req_sizes->addData(size);

    if (size == 0) return;

    StandardMem* link = linkFor(path);
    if (!link) {
        comp_->getComponent()->fatal(CALL_INFO, -1,
            "QuetzCore %" PRIu32 ": %s read to 0x%016" PRIx64 " but %s is not connected.\n",
            core_id_,
            (path == IssuePath::MMIO) ? "MMIO" : "cached",
            vaddr,
            (path == IssuePath::MMIO) ? "mmio_link" : "cache_link");
    }

    if (path == IssuePath::MMIO) {
        auto* req = new StandardMem::Read(vaddr, size, 0, vaddr);
        pending_txns_[req->getID()] = { req, comp_->getCurrentSimTime(tc_), true };
        pending_count_++;
        stats_.mmio_read_reqs->addData(1);
        link->send(req);
        return;
    }

    uint64_t addr      = vaddr;
    uint32_t remaining = size;
    uint32_t parts     = 0;

    while (remaining > 0) {
        uint64_t line_end = (addr & ~(cache_line_size_ - 1)) + cache_line_size_;
        uint32_t chunk    = (uint32_t)std::min<uint64_t>(line_end - addr,
                                                          (uint64_t)remaining);

        auto* req = new StandardMem::Read(addr, chunk, 0, addr);
        pending_txns_[req->getID()] = { req, comp_->getCurrentSimTime(tc_), false };
        pending_count_++;
        stats_.read_reqs->addData(1);
        link->send(req);

        addr      += chunk;
        remaining -= chunk;
        parts++;
    }

    if (parts > 1)
        stats_.split_reads->addData(parts - 1);

    if (check_addresses_ && size > (uint32_t)cache_line_size_)
        output_->verbose(CALL_INFO, 1, 0,
            "QuetzCore %" PRIu32 " READ vaddr=0x%016" PRIx64 " size=%" PRIu32
            " exceeds cache line size %" PRIu64 " (issued %" PRIu32 " sub-requests)\n",
            core_id_, vaddr, size, cache_line_size_, parts);
}

void MemRequestEmitter::issueWrite(uint64_t vaddr, uint32_t size, uint64_t /*pc*/,
                                   const uint8_t* raw_data, IssuePath path) {
    output_->verbose(CALL_INFO, 8, 0,
        "QuetzCore %" PRIu32 " WRITE vaddr=0x%016" PRIx64 " size=%" PRIu32
        " path=%s\n",
        core_id_, vaddr, size,
        (path == IssuePath::MMIO) ? "mmio" : "cached");

    if (path != IssuePath::MMIO)
        stats_.write_req_sizes->addData(size);

    if (size == 0) return;

    StandardMem* link = linkFor(path);
    if (!link) {
        comp_->getComponent()->fatal(CALL_INFO, -1,
            "QuetzCore %" PRIu32 ": %s write to 0x%016" PRIx64 " but %s is not connected.\n",
            core_id_,
            (path == IssuePath::MMIO) ? "MMIO" : "cached",
            vaddr,
            (path == IssuePath::MMIO) ? "mmio_link" : "cache_link");
    }

    static constexpr uint32_t kDataCap = (uint32_t)sizeof(QuetzCommand::data);

    if (path == IssuePath::MMIO) {
        uint32_t issue_size = size;
        if (size > kDataCap) {
            stats_.mmio_truncated_writes->addData(1);
            issue_size = kDataCap;
            output_->verbose(CALL_INFO, 1, 0,
                "QuetzCore %" PRIu32 " MMIO WRITE size=%" PRIu32
                " exceeds plugin data cap %" PRIu32 " — truncating.\n",
                core_id_, size, kDataCap);
        }

        std::vector<uint8_t> data(issue_size, 0);
        if (raw_data && issue_size > 0)
            memcpy(data.data(), raw_data, issue_size);

        auto* req = new StandardMem::Write(vaddr, issue_size, data, false, 0, vaddr);
        pending_txns_[req->getID()] = { req, comp_->getCurrentSimTime(tc_), true };
        pending_count_++;
        stats_.mmio_write_reqs->addData(1);
        link->send(req);
        return;
    }

    uint64_t addr        = vaddr;
    uint32_t remaining   = size;
    uint32_t data_offset = 0;
    uint32_t parts       = 0;

    while (remaining > 0) {
        uint64_t line_end = (addr & ~(cache_line_size_ - 1)) + cache_line_size_;
        uint32_t chunk    = (uint32_t)std::min<uint64_t>(line_end - addr,
                                                          (uint64_t)remaining);

        std::vector<uint8_t> data(chunk, 0);
        if (raw_data && data_offset < kDataCap) {
            uint32_t avail  = kDataCap - data_offset;
            uint32_t copy_n = (chunk < avail) ? chunk : avail;
            memcpy(data.data(), raw_data + data_offset, copy_n);
        }

        auto* req = new StandardMem::Write(addr, chunk, data, false, 0, addr);
        pending_txns_[req->getID()] = { req, comp_->getCurrentSimTime(tc_), false };
        pending_count_++;
        stats_.write_reqs->addData(1);
        link->send(req);

        addr        += chunk;
        data_offset += chunk;
        remaining   -= chunk;
        parts++;
    }

    if (parts > 1)
        stats_.split_writes->addData(parts - 1);

    if (check_addresses_ && size > (uint32_t)cache_line_size_)
        output_->verbose(CALL_INFO, 1, 0,
            "QuetzCore %" PRIu32 " WRITE vaddr=0x%016" PRIx64 " size=%" PRIu32
            " exceeds cache line size %" PRIu64 " (issued %" PRIu32 " sub-requests)\n",
            core_id_, vaddr, size, cache_line_size_, parts);
}

bool MemRequestEmitter::handleResponse(StandardMem::Request* resp,
                                       uint64_t& latency_out,
                                       bool& was_read_out,
                                       bool& was_mmio_out) {
    auto it = pending_txns_.find(resp->getID());
    if (it == pending_txns_.end()) {
        output_->verbose(CALL_INFO, 4, 0,
            "QuetzCore %" PRIu32 ": ignoring untracked response id %" PRIu64 "\n",
            core_id_, (uint64_t)resp->getID());
        delete resp;
        was_mmio_out = false;
        return false;
    }

    uint64_t issue = it->second.issue_cycle;
    uint64_t now   = comp_->getCurrentSimTime(tc_);
    latency_out    = (now >= issue) ? (now - issue) : 0;
    was_read_out   = (dynamic_cast<StandardMem::ReadResp*>(resp) != nullptr);
    was_mmio_out   = it->second.is_mmio;

    pending_txns_.erase(it);
    pending_count_--;
    delete resp;
    return true;
}
