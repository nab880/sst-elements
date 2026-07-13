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
#include "quetz_sink_device.h"

#include <fstream>
#include <inttypes.h>

using namespace SST;
using namespace SST::Interfaces;
using namespace SST::Quetz;

QuetzSinkDevice::QuetzSinkDevice(ComponentId_t id, Params& params)
    : Component(id),
      base_addr_(params.find<uint64_t>("base_addr", 0)),
      mmio_size_(params.find<uint64_t>("mmio_size", 0x100)),
      max_bytes_(params.find<uint64_t>("max_bytes", 0)),
      flushed_bytes_(0),
      accepted_(0),
      dropped_(0),
      handlers_(nullptr),
      iface_(nullptr)
{
    out.init("", params.find<int>("verbose", 0), 0, Output::STDOUT);

    sink_file_ = params.find<std::string>("sink_file", "");
    if (sink_file_.empty()) {
        out.fatal(CALL_INFO, -1,
            "%s: 'sink_file' is required (capture file for bytes pushed "
            "through REG_DATA).\n", getName().c_str());
    }
    // Fail configuration-time, not at finish(): create/truncate the file now
    // so an unwritable path aborts the run before hours of simulation.
    {
        std::ofstream f(sink_file_, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) {
            out.fatal(CALL_INFO, -1, "%s: cannot write sink_file '%s'.\n",
                getName().c_str(), sink_file_.c_str());
        }
    }

    std::string clockfreq = params.find<std::string>("clock", "1GHz");
    TimeConverter tc = getTimeConverter(clockfreq);

    iface_ = loadUserSubComponent<StandardMem>(
        "iface", ComponentInfo::SHARE_NONE, tc,
        new StandardMem::Handler<QuetzSinkDevice, &QuetzSinkDevice::handleEvent>(this));
    if (!iface_) {
        out.fatal(CALL_INFO, -1,
            "%s: no 'iface' subcomponent; load memHierarchy.standardInterface.\n",
            getName().c_str());
    }
    iface_->setMemoryMappedAddressRegion(base_addr_, mmio_size_);

    handlers_ = new mmioHandlers(this, &out);

    stat_bytes_accepted_ = registerStatistic<uint64_t>("bytes_accepted");
    stat_flushes_        = registerStatistic<uint64_t>("flushes");
    stat_truncates_      = registerStatistic<uint64_t>("truncates");
    stat_dropped_bytes_  = registerStatistic<uint64_t>("dropped_bytes");
    stat_wrong_direction_accesses_ =
        registerStatistic<uint64_t>("wrong_direction_accesses");
    stat_bad_offset_accesses_ = registerStatistic<uint64_t>("bad_offset_accesses");

    out.verbose(CALL_INFO, 1, 0,
        "%s: MMIO [0x%" PRIx64 ", 0x%" PRIx64 ") capturing to '%s'%s\n",
        getName().c_str(), base_addr_, base_addr_ + mmio_size_,
        sink_file_.c_str(), max_bytes_ ? " (capped)" : "");
}

void QuetzSinkDevice::init(unsigned int phase) { iface_->init(phase); }
void QuetzSinkDevice::setup() { iface_->setup(); }

void QuetzSinkDevice::finish() {
    writeFile(false);
}

// Append only the suffix not persisted by an earlier flush. CTRL truncate
// resets both the file and the persisted-prefix cursor.
void QuetzSinkDevice::writeFile(bool truncate_only) {
    std::ios::openmode mode = std::ios::binary |
        (truncate_only ? std::ios::trunc : std::ios::app);
    std::ofstream f(sink_file_, mode);
    if (!f.is_open()) {
        out.fatal(CALL_INFO, -1, "%s: cannot write sink_file '%s'.\n",
            getName().c_str(), sink_file_.c_str());
    }
    if (truncate_only) {
        flushed_bytes_ = 0;
        return;
    }
    if (flushed_bytes_ > captured_.size())
        out.fatal(CALL_INFO, -1, "%s: sink flush cursor exceeds capture.\n",
            getName().c_str());
    size_t pending = captured_.size() - flushed_bytes_;
    if (pending) {
        f.write(reinterpret_cast<const char*>(captured_.data() + flushed_bytes_),
                (std::streamsize)pending);
        if (!f)
            out.fatal(CALL_INFO, -1, "%s: failed writing sink_file '%s'.\n",
                getName().c_str(), sink_file_.c_str());
        flushed_bytes_ = captured_.size();
    }
}

void QuetzSinkDevice::handleEvent(StandardMem::Request* req) {
    req->handle(handlers_);
    delete req;
}

void QuetzSinkDevice::mmioHandlers::handle(StandardMem::Read* read) {
    uint64_t offset = read->pAddr - dev->base_addr_;
    uint64_t value = 0;

    if (offset == REG_STATUS || offset == REG_SEQ) {
        value = dev->accepted_;
    } else if (offset == REG_DATA || offset == REG_CTRL) {
        dev->stat_wrong_direction_accesses_->addData(1);
    } else {
        dev->stat_bad_offset_accesses_->addData(1);
    }

    std::vector<uint8_t> payload;
    for (size_t i = 0; i < read->size; i++) {
        payload.push_back((uint8_t)(value & 0xFF));
        value >>= 8;
    }
    StandardMem::ReadResp* resp =
        static_cast<StandardMem::ReadResp*>(read->makeResponse());
    resp->data = payload;
    dev->iface_->send(resp);
}

void QuetzSinkDevice::mmioHandlers::handle(StandardMem::Write* write) {
    uint64_t offset = write->pAddr - dev->base_addr_;

    if (offset == REG_DATA) {
        // Push exactly write-size bytes: the payload already arrives
        // low-byte-first (value semantics through the sync mailbox), so the
        // bytes are captured in guest push order with no unpacking needed.
        for (size_t i = 0; i < write->data.size(); i++) {
            if (dev->max_bytes_ && dev->captured_.size() >= dev->max_bytes_) {
                dev->dropped_++;
                dev->stat_dropped_bytes_->addData(1);
                continue;
            }
            dev->captured_.push_back(write->data[i]);
            dev->accepted_++;
            dev->stat_bytes_accepted_->addData(1);
        }
    } else if (offset == REG_CTRL) {
        uint64_t value = 0;
        for (int i = (int)write->data.size() - 1; i >= 0; i--) {
            value <<= 8;
            value |= write->data[(size_t)i];
        }
        if (value == 1) {
            dev->writeFile(false);
            dev->stat_flushes_->addData(1);
            out->verbose(CALL_INFO, 2, 0,
                "%s: flushed %zu bytes to '%s'\n", dev->getName().c_str(),
                dev->captured_.size(), dev->sink_file_.c_str());
        } else if (value == 2) {
            dev->captured_.clear();
            dev->accepted_ = 0;
            dev->writeFile(true);
            dev->stat_truncates_->addData(1);
            out->verbose(CALL_INFO, 2, 0,
                "%s: capture truncated/restarted\n", dev->getName().c_str());
        }
    } else if (offset == REG_STATUS || offset == REG_SEQ) {
        dev->stat_wrong_direction_accesses_->addData(1);
    } else {
        dev->stat_bad_offset_accesses_->addData(1);
    }

    if (!write->posted)
        dev->iface_->send(write->makeResponse());
}

void QuetzSinkDevice::printStatus(Output& statusOut) {
    statusOut.output("Quetz::QuetzSinkDevice %s\n", getName().c_str());
    statusOut.output("    base_addr=0x%" PRIx64 " accepted=%" PRIu64
        " dropped=%" PRIu64 " file=%s\n",
        base_addr_, accepted_, dropped_, sink_file_.c_str());
    iface_->printStatus(statusOut);
    statusOut.output("End Quetz::QuetzSinkDevice\n\n");
}
