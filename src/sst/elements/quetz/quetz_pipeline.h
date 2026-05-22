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

#ifndef _H_SST_QUETZ_PIPELINE
#define _H_SST_QUETZ_PIPELINE

#include <queue>

#include "quetz_mem_issue.h"
#include "quetz_pipeline_api.h"

namespace SST {
namespace Quetz {

class QuetzEventPipeline {
public:
    QuetzEventPipeline(QuetzCoreContext&              ctx,
                       PipelineInput*                input,
                       PipelineFilter*               filter,
                       PipelineTransform*              transform,
                       PipelineOutput*               output);

    void tick();
    void finish();

    void setMemLink(SST::Interfaces::StandardMem* link) {
        output_->setMemLink(link);
    }
    bool handleResponse(SST::Interfaces::StandardMem::Request* resp,
                        uint64_t& latency_out,
                        bool&     was_read_out) {
        return output_->handleResponse(resp, latency_out, was_read_out);
    }

    bool     isHalted()     const { return halted_; }
    uint32_t pendingCount() const { return output_->pendingCount(); }

private:
    bool checkMaxInsts();

    QuetzCoreContext&         ctx_;
    PipelineInput*            input_;
    PipelineFilter*           filter_;
    PipelineTransform*        transform_;
    PipelineOutput*           output_;
    MemRequestEmitter         emitter_;

    uint64_t                  inst_count_;
    bool                      halted_;
    std::queue<PipelineEvent> coreQ_;
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_PIPELINE
