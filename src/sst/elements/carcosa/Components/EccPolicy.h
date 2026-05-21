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
#include <map>
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

// Resolution precedence:
//   (kernel,region) > region > kernel > uniform
// kernel "*" means "any kernel"; region "*" or "" means "any region".
// CSV entry forms accepted:
//   KERNEL:scheme:ber:c_ps:d_ps:e_ps
//   KERNEL@REGION:scheme:ber:c_ps:d_ps:e_ps
//   *@REGION:scheme:ber:c_ps:d_ps:e_ps    (region-only)
//   KERNEL@*:scheme:ber:c_ps:d_ps:e_ps    (same as kernel-only)
class EccPolicyTable {
public:
    EccPolicyTable() {
        for (int i = 0; i < NUM_STATES; ++i) {
            per_kernel_[i].inherits_uniform = true;
        }
    }

    void setUniform(const EccPolicyEntry& e) { uniform_ = e; uniform_.inherits_uniform = false; }
    const EccPolicyEntry& uniform() const { return uniform_; }

    // Walk every concrete entry in the table (uniform + per-kernel + per-
    // region + per-(kernel,region)). The callback receives a human-readable
    // tag describing where the entry came from, e.g. "uniform",
    // "kernel=PREFILL", "region=KV_CACHE", "kernel=ACTUATE@region=ACTION".
    // Used by EccGuard::setup() to surface BER-range warnings up front.
    template <typename Fn>
    void forEachEntry(Fn&& fn) const {
        fn(std::string("uniform"), uniform_);
        for (int i = 0; i < NUM_STATES; ++i) {
            const EccPolicyEntry& e = per_kernel_[i];
            if (!e.inherits_uniform) {
                std::string tag = std::string("kernel=") + vlaStateName(i);
                fn(tag, e);
            }
        }
        for (const auto& kv : per_region_) {
            fn(std::string("region=") + kv.first, kv.second);
        }
        for (const auto& kv : per_kernel_region_) {
            fn(std::string("kernel@region=") + kv.first, kv.second);
        }
    }

    // Backwards-compatible kernel-only resolver. Region-unaware callers stay on this path.
    const EccPolicyEntry& effectiveFor(int kernel_id) const {
        if (kernel_id < 0 || kernel_id >= NUM_STATES) return uniform_;
        const EccPolicyEntry& e = per_kernel_[kernel_id];
        return e.inherits_uniform ? uniform_ : e;
    }

    // (kernel,region) > region > kernel > uniform.
    // region_name "" or unmatched falls through to kernel-only.
    const EccPolicyEntry& effectiveFor(int kernel_id,
                                       const std::string& region_name) const {
        if (!region_name.empty()) {
            if (kernel_id >= 0 && kernel_id < NUM_STATES) {
                auto it = per_kernel_region_.find(makeComboKey(kernel_id, region_name));
                if (it != per_kernel_region_.end()) return it->second;
            }
            auto rit = per_region_.find(region_name);
            if (rit != per_region_.end()) return rit->second;
        }
        return effectiveFor(kernel_id);
    }

    void setPerKernel(int kernel_id, const EccPolicyEntry& e) {
        if (kernel_id < 0 || kernel_id >= NUM_STATES) return;
        per_kernel_[kernel_id] = e;
        per_kernel_[kernel_id].inherits_uniform = false;
    }

    void setPerRegion(const std::string& region_name, const EccPolicyEntry& e) {
        if (region_name.empty()) return;
        EccPolicyEntry copy = e;
        copy.inherits_uniform = false;
        per_region_[region_name] = copy;
    }

    void setPerKernelRegion(int kernel_id, const std::string& region_name,
                            const EccPolicyEntry& e) {
        if (kernel_id < 0 || kernel_id >= NUM_STATES) return;
        if (region_name.empty()) return;
        EccPolicyEntry copy = e;
        copy.inherits_uniform = false;
        per_kernel_region_[makeComboKey(kernel_id, region_name)] = copy;
    }

    // CSV per entry. See class doc for accepted forms.
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

            // Tag may be KERNEL, KERNEL@REGION, *@REGION, or KERNEL@*.
            std::string kernel_tok;
            std::string region_tok;
            splitAt(parts[0], kernel_tok, region_tok);

            bool kernel_any = (kernel_tok == "*" || kernel_tok.empty());
            bool region_any = (region_tok == "*" || region_tok.empty());

            int kid = -1;
            if (!kernel_any) {
                kid = stateIdFromName(kernel_tok);
                if (kid < 0) {
                    errors.push_back("ecc_kernel_policy: unknown kernel '" + kernel_tok + "'");
                    continue;
                }
            }

            EccPolicyEntry e;
            e.inherits_uniform = false;

            if (parts.size() >= 2) {
                if (!eccSchemeFromString(parts[1], e.scheme)) {
                    errors.push_back("ecc_kernel_policy: unknown scheme '" + parts[1] + "' for '" + parts[0] + "'");
                    continue;
                }
            }
            if (parts.size() >= 3) e.ber                    = parseDouble(parts[2]);
            if (parts.size() >= 4) e.correctable_latency_ps = parseUInt64(parts[3]);
            if (parts.size() >= 5) e.due_latency_ps         = parseUInt64(parts[4]);
            if (parts.size() >= 6) e.escape_latency_ps      = parseUInt64(parts[5]);

            if (!kernel_any && !region_any) {
                setPerKernelRegion(kid, region_tok, e);
            } else if (!region_any) {
                setPerRegion(region_tok, e);
            } else if (!kernel_any) {
                setPerKernel(kid, e);
            } else {
                errors.push_back("ecc_kernel_policy: '*@*' not allowed; use ecc_scheme/ber for the uniform fallback");
                continue;
            }
            ++parsed;
        }
        return parsed;
    }

private:
    EccPolicyEntry uniform_{};
    EccPolicyEntry per_kernel_[NUM_STATES]{};
    std::map<std::string, EccPolicyEntry> per_region_;
    std::map<std::string, EccPolicyEntry> per_kernel_region_;

    static std::string makeComboKey(int kernel_id, const std::string& region) {
        return std::to_string(kernel_id) + "@" + region;
    }

    static void splitAt(const std::string& tag, std::string& kernel,
                        std::string& region) {
        auto pos = tag.find('@');
        if (pos == std::string::npos) {
            kernel = tag;
            region.clear();
        } else {
            kernel = tag.substr(0, pos);
            region = tag.substr(pos + 1);
        }
    }

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
