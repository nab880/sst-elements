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
#include "quetz_config_manager.h"

#include <inttypes.h>
#include <sstream>

using namespace SST;
using namespace SST::Quetz;

namespace SST {
namespace Quetz {
const std::vector<QuetzPlatformProfile>& quetzPlatformRegistry();
} // namespace Quetz
} // namespace SST

QuetzConfigManager QuetzConfigManager::fromParams(Params& params,
                                                  SST::Output* out) {
    QuetzConfigManager mgr;

    std::string platform = params.find<std::string>("platform", "");
    const QuetzPlatformProfile* profile = nullptr;
    if (!platform.empty()) {
        profile = findPlatform(platform);
        if (!profile) {
            out->fatal(CALL_INFO, -1,
                "Unknown platform preset '%s'.  Available presets: %s\n",
                platform.c_str(), platformNames().c_str());
        }
        out->verbose(CALL_INFO, 1, 0,
            "Applying platform preset '%s' (%s).\n",
            profile->name.c_str(), profile->description.c_str());
        applyProfile(params, *profile);
    }

    mgr.cfg_ = QuetzConfig::fromParams(params, out);
    if (profile && mgr.cfg_.region_handlers.empty())
        mgr.cfg_.region_handlers = profile->region_handlers;
    mgr.validateExtended(out);

    return mgr;
}

void QuetzConfigManager::applyProfile(Params& params,
                                      const QuetzPlatformProfile& profile) {
    for (const auto& kv : profile.param_defaults)
        params.insert(kv.first, kv.second, /*overwrite=*/false);
}

void QuetzConfigManager::validateExtended(SST::Output* out) const {
    for (size_t i = 0; i < cfg_.region_handlers.size(); i++) {
        const RegionHandlerPreset& preset = cfg_.region_handlers[i];
        uint64_t start = 0, end = 0;
        for (const auto& kv : preset.params) {
            if (kv.first == "start")
                start = std::stoull(kv.second, nullptr, 0);
            else if (kv.first == "end")
                end = std::stoull(kv.second, nullptr, 0);
        }
        if (start > end) {
            out->fatal(CALL_INFO, -1,
                "region_handlers[%" PRIu64 "] '%s': start 0x%016" PRIx64
                " > end 0x%016" PRIx64 "\n",
                (uint64_t)i, preset.type.c_str(), start, end);
        }
    }

    for (size_t i = 0; i < cfg_.region_handlers.size(); i++) {
        const RegionHandlerPreset& a = cfg_.region_handlers[i];
        uint64_t a_start = 0, a_end = 0, b_start = 0, b_end = 0;
        for (const auto& kv : a.params) {
            if (kv.first == "start") a_start = std::stoull(kv.second, nullptr, 0);
            if (kv.first == "end")   a_end   = std::stoull(kv.second, nullptr, 0);
        }
        for (size_t j = i + 1; j < cfg_.region_handlers.size(); j++) {
            const RegionHandlerPreset& b = cfg_.region_handlers[j];
            b_start = b_end = 0;
            for (const auto& kv : b.params) {
                if (kv.first == "start") b_start = std::stoull(kv.second, nullptr, 0);
                if (kv.first == "end")   b_end   = std::stoull(kv.second, nullptr, 0);
            }
            if (a_start <= b_end && b_start <= a_end) {
                out->verbose(CALL_INFO, 1, 0,
                    "Region handlers '%s' and '%s' overlap; "
                    "first match (index %zu) wins.\n",
                    a.type.c_str(), b.type.c_str(), i);
            }
        }
    }
}

std::string QuetzConfigManager::platformNames() {
    std::ostringstream os;
    bool first = true;
    for (const auto& p : quetzPlatformRegistry()) {
        if (!first) os << ", ";
        os << p.name;
        first = false;
    }
    return os.str();
}

const QuetzPlatformProfile*
QuetzConfigManager::findPlatform(const std::string& name) {
    for (const auto& p : quetzPlatformRegistry())
        if (p.name == name)
            return &p;
    return nullptr;
}
