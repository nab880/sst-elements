// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain
// rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#ifndef _H_SST_QUETZ_ACCELERATOR_EVENT_WRITER
#define _H_SST_QUETZ_ACCELERATOR_EVENT_WRITER

#include <fstream>
#include <stdint.h>
#include <string>

namespace SST {
namespace Quetz {

// Durable-at-the-C++-stream-boundary JSONL writer for the accelerator
// lifecycle contract. Identifiers are restricted to the event schema's
// alphabet, so they can be inserted into JSON without an escaping ambiguity.
// The writer deliberately reports only lifecycle state: DMA visibility is
// established by the device state machine, not claimed as a separate event.
class AcceleratorEventWriter {
public:
    AcceleratorEventWriter() : sequence_(0), enabled_(false) {}

    static bool isIdentifier(const std::string& value)
    {
        if (value.empty() || !isLowerOrDigit(value[0]))
            return false;
        for (char c : value) {
            if (!isLowerOrDigit(c) && c != '.' && c != '_' && c != '-')
                return false;
        }
        return true;
    }

    bool configure(const std::string& path, const std::string& source,
                   const std::string& operation, std::string& error)
    {
        error.clear();
        if (path.empty())
            return true;
        if (!isIdentifier(source)) {
            error = "event_source must match ^[a-z0-9][a-z0-9._-]*$";
            return false;
        }
        if (!isIdentifier(operation)) {
            error = "event_operation must match ^[a-z0-9][a-z0-9._-]*$";
            return false;
        }

        stream_.open(path, std::ios::out | std::ios::trunc);
        if (!stream_.is_open()) {
            error = "cannot open accelerator event file '" + path + "'";
            return false;
        }
        source_ = source;
        operation_ = operation;
        sequence_ = 0;
        enabled_ = true;
        return true;
    }

    bool enabled() const { return enabled_; }

    bool emitRequested(uint64_t sim_time_ns, uint64_t operation_id,
                       std::string& error)
    {
        return emit("accelerator-requested", sim_time_ns, operation_id, "", error);
    }

    bool emitCompleted(uint64_t sim_time_ns, uint64_t operation_id,
                       std::string& error)
    {
        return emit("accelerator-completed", sim_time_ns, operation_id, "", error);
    }

    bool emitError(uint64_t sim_time_ns, uint64_t operation_id,
                   const std::string& code, std::string& error)
    {
        if (!isIdentifier(code)) {
            error = "accelerator error code must be a schema identifier";
            return false;
        }
        return emit("accelerator-error", sim_time_ns, operation_id, code, error);
    }

private:
    static bool isLowerOrDigit(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    }

    bool emit(const char* kind, uint64_t sim_time_ns, uint64_t operation_id,
              const std::string& code, std::string& error)
    {
        error.clear();
        if (!enabled_)
            return true;

        stream_ << "{\"schema_version\":1,\"sequence\":" << sequence_
                << ",\"sim_time_ns\":" << sim_time_ns
                << ",\"source\":\"" << source_
                << "\",\"kind\":\"" << kind
                << "\",\"payload\":{\"operation_id\":" << operation_id
                << ",\"operation\":\"" << operation_ << "\"";
        if (!code.empty())
            stream_ << ",\"code\":\"" << code << "\"";
        stream_ << "}}\n";
        stream_.flush();
        if (!stream_) {
            error = "failed to write and flush accelerator event record";
            return false;
        }
        ++sequence_;
        return true;
    }

    std::ofstream stream_;
    std::string source_;
    std::string operation_;
    uint64_t sequence_;
    bool enabled_;
};

} // namespace Quetz
} // namespace SST

#endif
