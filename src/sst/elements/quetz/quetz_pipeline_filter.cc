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

#include "quetz_mem_access.h"
#include "quetz_pipeline_api.h"

#include <memory>

using namespace SST;
using namespace SST::Quetz;

class DefaultPipelineFilter : public PipelineFilter {
public:
    SST_ELI_REGISTER_SUBCOMPONENT(
        DefaultPipelineFilter,
        "quetz",
        "DefaultPipelineFilter",
        SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "Apply MemRegionTable policy before memory timing.",
        SST::Quetz::PipelineFilter)

    DefaultPipelineFilter(ComponentId_t id, Params& params)
        : PipelineFilter(id, params)
    {}

    void configure(const QuetzCoreContext& ctx) override {
        if (ctx.region_table)
            strategy_.reset(new RegionTableMemAccessStrategy(*ctx.region_table));
    }

    bool handle(const QuetzCommand& cmd, QuetzCoreStats& stats,
                MemRegionHandler::Action& region_action_out) override {
        if (!strategy_) {
            region_action_out = MemRegionHandler::Action::FORWARD;
            return false;
        }
        region_action_out = strategy_->handleMemoryAccess(cmd, stats);
        return (region_action_out == MemRegionHandler::Action::CONSUME);
    }

    void finish(SST::Output* out, uint32_t core_id) override {
        if (strategy_)
            strategy_->finish(out, core_id);
    }

private:
    std::unique_ptr<RegionTableMemAccessStrategy> strategy_;
};
