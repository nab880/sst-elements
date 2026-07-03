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
#include "quetz_stream_device.h"

#include <fstream>
#include <inttypes.h>

using namespace SST;
using namespace SST::Interfaces;
using namespace SST::Quetz;

QuetzStreamDevice::QuetzStreamDevice(ComponentId_t id, Params& params)
    : Component(id),
      base_addr_(params.find<uint64_t>("base_addr", 0)),
      mmio_size_(params.find<uint64_t>("mmio_size", 0x100)),
      pos_(0),
      handlers_(nullptr),
      iface_(nullptr)
{
    out.init("", params.find<int>("verbose", 0), 0, Output::STDOUT);

    std::string stream_file = params.find<std::string>("stream_file", "");
    if (stream_file.empty()) {
        out.fatal(CALL_INFO, -1,
            "%s: 'stream_file' is required (binary fixture replayed via REG_DATA).\n",
            getName().c_str());
    }
    std::ifstream f(stream_file, std::ios::binary);
    if (!f.is_open()) {
        out.fatal(CALL_INFO, -1, "%s: cannot open stream_file '%s'.\n",
            getName().c_str(), stream_file.c_str());
    }
    stream_.assign(std::istreambuf_iterator<char>(f),
                   std::istreambuf_iterator<char>());
    if (stream_.empty()) {
        out.fatal(CALL_INFO, -1, "%s: stream_file '%s' is empty.\n",
            getName().c_str(), stream_file.c_str());
    }

    std::string clockfreq = params.find<std::string>("clock", "1GHz");
    TimeConverter tc = getTimeConverter(clockfreq);

    iface_ = loadUserSubComponent<StandardMem>(
        "iface", ComponentInfo::SHARE_NONE, tc,
        new StandardMem::Handler<QuetzStreamDevice, &QuetzStreamDevice::handleEvent>(this));
    if (!iface_) {
        out.fatal(CALL_INFO, -1,
            "%s: no 'iface' subcomponent; load memHierarchy.standardInterface.\n",
            getName().c_str());
    }
    iface_->setMemoryMappedAddressRegion(base_addr_, mmio_size_);

    handlers_ = new mmioHandlers(this, &out);

    stat_data_reads_       = registerStatistic<uint64_t>("data_reads");
    stat_bytes_delivered_  = registerStatistic<uint64_t>("bytes_delivered");
    stat_status_polls_     = registerStatistic<uint64_t>("status_polls");
    stat_underruns_        = registerStatistic<uint64_t>("underruns");
    stat_rewinds_          = registerStatistic<uint64_t>("rewinds");
    stat_wrong_direction_accesses_ =
        registerStatistic<uint64_t>("wrong_direction_accesses");
    stat_bad_offset_accesses_ = registerStatistic<uint64_t>("bad_offset_accesses");

    out.verbose(CALL_INFO, 1, 0,
        "%s: MMIO [0x%" PRIx64 ", 0x%" PRIx64 ") streaming %zu bytes from '%s'\n",
        getName().c_str(), base_addr_, base_addr_ + mmio_size_,
        stream_.size(), stream_file.c_str());
}

void QuetzStreamDevice::init(unsigned int phase) { iface_->init(phase); }
void QuetzStreamDevice::setup() { iface_->setup(); }

void QuetzStreamDevice::handleEvent(StandardMem::Request* req) {
    req->handle(handlers_);
    delete req;
}

void QuetzStreamDevice::mmioHandlers::handle(StandardMem::Read* read) {
    uint64_t offset = read->pAddr - dev->base_addr_;
    uint64_t value = 0;

    if (offset == REG_STATUS) {
        value = dev->stream_.size() - dev->pos_;
        dev->stat_status_polls_->addData(1);
    } else if (offset == REG_DATA) {
        dev->stat_data_reads_->addData(1);
        if (dev->pos_ >= dev->stream_.size()) {
            dev->stat_underruns_->addData(1);
        } else {
            unsigned n = 0;
            while (n < 4 && dev->pos_ < dev->stream_.size()) {
                value |= (uint64_t)dev->stream_[dev->pos_++] << (8 * n);
                n++;
            }
            dev->stat_bytes_delivered_->addData(n);
        }
    } else if (offset == REG_SEQ) {
        value = dev->pos_;
    } else if (offset == REG_CTRL) {
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

void QuetzStreamDevice::mmioHandlers::handle(StandardMem::Write* write) {
    uint64_t offset = write->pAddr - dev->base_addr_;

    if (offset == REG_CTRL) {
        uint64_t value = 0;
        for (int i = (int)write->data.size() - 1; i >= 0; i--) {
            value <<= 8;
            value |= write->data[(size_t)i];
        }
        if (value == 1) {
            dev->pos_ = 0;
            dev->stat_rewinds_->addData(1);
            out->verbose(CALL_INFO, 2, 0, "%s: stream rewound\n",
                dev->getName().c_str());
        }
    } else if (offset == REG_STATUS || offset == REG_DATA || offset == REG_SEQ) {
        dev->stat_wrong_direction_accesses_->addData(1);
    } else {
        dev->stat_bad_offset_accesses_->addData(1);
    }

    if (!write->posted)
        dev->iface_->send(write->makeResponse());
}

void QuetzStreamDevice::printStatus(Output& statusOut) {
    statusOut.output("Quetz::QuetzStreamDevice %s\n", getName().c_str());
    statusOut.output("    base_addr=0x%" PRIx64 " size=%zu pos=%zu\n",
        base_addr_, stream_.size(), pos_);
    iface_->printStatus(statusOut);
    statusOut.output("End Quetz::QuetzStreamDevice\n\n");
}
