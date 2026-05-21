// Copyright 2009-2024 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2024, NTESS
// All rights reserved.
//
// Portions are copyright of other developers:
// See the file CONTRIBUTORS.TXT in the top level directory
// of the distribution for more information.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#ifndef CARCOSA_INTERCEPTIONAGENT_API_H
#define CARCOSA_INTERCEPTIONAGENT_API_H

#include <sst/core/subcomponent.h>
#include <sst/core/link.h>
#include <sst/elements/memHierarchy/memEvent.h>
#include <sst/elements/memHierarchy/memTypes.h>
#include "sst/elements/carcosa/Components/HaliEvent.h"
#include <cinttypes>
#include <cstdio>
#include <cstdint>

namespace SST {
namespace Carcosa {

class InterceptionAgentAPI : public SST::SubComponent
{
public:
    SST_ELI_REGISTER_SUBCOMPONENT_API(SST::Carcosa::InterceptionAgentAPI)

    InterceptionAgentAPI(ComponentId_t id, Params& params) : SubComponent(id) {}
    virtual ~InterceptionAgentAPI() {}

    /** Intercepted MemEvent: respond on highlink; return true if handled (not forwarded). */
    virtual bool handleInterceptedEvent(SST::MemHierarchy::MemEvent* ev, SST::Link* highlink) = 0;

    virtual void notifyPartnerDone(unsigned iteration) { (void)iteration; }

    // Default routes "done" to notifyPartnerDone for PingPong/FourState; VLA agents
    // override this to consume "cmd"/"seqlen"/"exit"/"done" tags. Hali calls this for
    // every run-phase HaliEvent when an interceptionAgent is configured.
    virtual void handleRingEvent(SST::Carcosa::HaliEvent* ev) {
        if (ev && ev->getStr() == "done") {
            notifyPartnerDone(ev->getNum());
        }
    }

    virtual void agentSetup() {}

    virtual void setRingLink(SST::Link* leftLink) { (void)leftLink; }

    virtual void setInterceptBase(uint64_t base) { (void)base; }

    virtual void setHighlink(SST::Link* highlink) { (void)highlink; }

    InterceptionAgentAPI() {}
    ImplementVirtualSerializable(SST::Carcosa::InterceptionAgentAPI);

protected:
    /** Log, delete ev, return true; no response (reads hang). For unreachable offsets only. */
    bool warnAndDropUnknownIntercept(SST::MemHierarchy::MemEvent* ev,
                                     uint64_t base) {
        uint64_t addr = static_cast<uint64_t>(ev->getAddr());
        uint64_t off  = addr - base;
        int cmdIdx = static_cast<int>(ev->getCmd());
        const char* cmdName = SST::MemHierarchy::CommandString[cmdIdx];
        fprintf(stderr,
                "[CARCOSA WARN] %s: unhandled intercepted access "
                "cmd=%s addr=0x%" PRIx64 " offset=+0x%" PRIx64
                " (event dropped, no response sent)\n",
                getName().c_str(), cmdName, addr, off);
        delete ev;
        return true;
    }
};

} // namespace Carcosa
} // namespace SST

#endif /* CARCOSA_INTERCEPTIONAGENT_API_H */
