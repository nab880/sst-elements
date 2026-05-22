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

/**
 * quetz_config_manager.h — centralized configuration manager for Quetz.
 */

#ifndef _H_SST_QUETZ_CONFIG_MANAGER
#define _H_SST_QUETZ_CONFIG_MANAGER

#include <sst/core/output.h>
#include <sst/core/params.h>

#include <string>
#include <utility>
#include <vector>

#include "quetz_config.h"

namespace SST {
namespace Quetz {

struct QuetzPlatformProfile {
    std::string name;
    std::string description;
    std::vector<std::pair<std::string, std::string>> param_defaults;
    /** Default region_handler subcomponents when SDL does not populate slots. */
    std::vector<RegionHandlerPreset> region_handlers;
};

class QuetzConfigManager {
public:
    static QuetzConfigManager fromParams(Params& params, SST::Output* out);

    const QuetzConfig& config() const { return cfg_; }

    static const QuetzPlatformProfile* findPlatform(const std::string& name);
    static std::string platformNames();

private:
    QuetzConfigManager() = default;

    static void applyProfile(Params& params,
                             const QuetzPlatformProfile& profile);

    void validateExtended(SST::Output* out) const;

    QuetzConfig cfg_;
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_CONFIG_MANAGER
