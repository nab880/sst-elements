// Copyright 2009-2025 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2025, NTESS
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#ifndef SST_ELEMENTS_CARCOSA_ECC_POLICY_H
#define SST_ELEMENTS_CARCOSA_ECC_POLICY_H

#include "sst/elements/carcosa/Components/EccScheme.h"
#include "sst/elements/carcosa/VLA-Example/Components/vla-fsm.h"
#include <cctype>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace SST {
namespace Carcosa {

// Latencies in picoseconds; ber is per-bit error probability per access.
struct EccPolicyEntry {
    EccScheme scheme                    = EccScheme::NONE;
    double    ber                       = 0.0;
    uint64_t  correctable_latency_ps    = 0;
    uint64_t  due_latency_ps            = 0;
    uint64_t  escape_latency_ps         = 0;
    bool      inherits_uniform          = true;
};

class EccPolicyTable {
public:
    EccPolicyTable() {
        for (int i = 0; i < NUM_STATES; ++i) {
            per_kernel_[i].inherits_uniform = true;
        }
    }

    void setUniform(const EccPolicyEntry& e) { uniform_ = e; uniform_.inherits_uniform = false; }
    const EccPolicyEntry& uniform() const { return uniform_; }

    const EccPolicyEntry& effectiveFor(int kernel_id) const {
        if (kernel_id < 0 || kernel_id >= NUM_STATES) return uniform_;
        const EccPolicyEntry& e = per_kernel_[kernel_id];
        return e.inherits_uniform ? uniform_ : e;
    }

    void setPerKernel(int kernel_id, const EccPolicyEntry& e) {
        if (kernel_id < 0 || kernel_id >= NUM_STATES) return;
        per_kernel_[kernel_id] = e;
        per_kernel_[kernel_id].inherits_uniform = false;
    }

    // CSV format per entry: KERNEL_NAME:scheme:ber:correctable_ps:due_ps:escape_ps
    int parseCsv(const std::string& csv, std::vector<std::string>& errors) {
        int parsed = 0;
        if (csv.empty()) return 0;

        std::string buf;
        std::istringstream ss(csv);
        while (std::getline(ss, buf, ',')) {
            trim(buf);
            if (buf.empty()) continue;

            std::vector<std::string> parts = splitColon(buf);
            if (parts.size() < 2) {
                errors.push_back("ecc_kernel_policy: malformed entry '" + buf + "'");
                continue;
            }
            for (auto& p : parts) trim(p);

            int kid = stateIdFromName(parts[0]);
            if (kid < 0) {
                errors.push_back("ecc_kernel_policy: unknown kernel '" + parts[0] + "'");
                continue;
            }

            EccPolicyEntry e;
            e.inherits_uniform = false;

            if (parts.size() >= 2) {
                if (!eccSchemeFromString(parts[1], e.scheme)) {
                    errors.push_back("ecc_kernel_policy: unknown scheme '" + parts[1] + "' for " + parts[0]);
                    continue;
                }
            }
            if (parts.size() >= 3) e.ber                    = parseDouble(parts[2]);
            if (parts.size() >= 4) e.correctable_latency_ps = parseUInt64(parts[3]);
            if (parts.size() >= 5) e.due_latency_ps         = parseUInt64(parts[4]);
            if (parts.size() >= 6) e.escape_latency_ps      = parseUInt64(parts[5]);

            per_kernel_[kid] = e;
            ++parsed;
        }
        return parsed;
    }

private:
    EccPolicyEntry uniform_{};
    EccPolicyEntry per_kernel_[NUM_STATES]{};

    static void trim(std::string& s) {
        size_t b = 0;
        while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
        size_t e = s.size();
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
        s = s.substr(b, e - b);
    }

    static std::vector<std::string> splitColon(const std::string& s) {
        std::vector<std::string> out;
        std::string buf;
        std::istringstream ss(s);
        while (std::getline(ss, buf, ':')) out.push_back(buf);
        return out;
    }

    static int stateIdFromName(const std::string& n) {
        for (int i = 0; i < NUM_STATES; ++i) {
            if (n == vlaStateName(i)) return i;
        }
        return -1;
    }

    static double parseDouble(const std::string& s) {
        try { return std::stod(s); } catch (...) { return 0.0; }
    }
    static uint64_t parseUInt64(const std::string& s) {
        try { return static_cast<uint64_t>(std::stoull(s)); } catch (...) { return 0; }
    }
};

} // namespace Carcosa
} // namespace SST

#endif /* SST_ELEMENTS_CARCOSA_ECC_POLICY_H */
