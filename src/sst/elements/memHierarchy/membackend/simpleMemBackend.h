// Copyright 2009-2017 Sandia Corporation. Under the terms
// of Contract DE-NA0003525 with Sandia Corporation, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2017, Sandia Corporation
// All rights reserved.
//
// Portions are copyright of other developers:
// See the file CONTRIBUTORS.TXT in the top level directory
// the distribution for more information.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.


#ifndef _H_SST_MEMH_SIMPLE_MEM_BACKEND
#define _H_SST_MEMH_SIMPLE_MEM_BACKEND

#include "membackend/memBackend.h"

namespace SST {
namespace MemHierarchy {

class SimpleMemory : public SimpleMemBackend {
public:
/* Element Library Info */
    SST_ELI_REGISTER_SUBCOMPONENT(SimpleMemory, "memHierarchy", "simpleMem", SST_ELI_ELEMENT_VERSION(1,0,0),
            "Basic constant-access-time memory timing model", "SST::MemHierarchy::MemBackend")
    
    SST_ELI_DOCUMENT_PARAMS(
            /* Inherited from MemBackend */
            {"debug_level",     "(uint) Debugging level: 0 (no output) to 10 (all output). Output also requires that SST Core be compiled with '--enable-debug'", "0"},
            {"debug_mask",      "(uint) Mask on debug_level", "0"},
            {"debug_location",  "(uint) 0: No debugging, 1: STDOUT, 2: STDERR, 3: FILE", "0"},
            {"clock", "(string) Clock frequency - inherited from MemController", NULL},
            {"max_requests_per_cycle", "(int) Maximum number of requests to accept each cycle. Use 0 or -1 for unlimited.", "-1"},
            {"request_width", "(int) Maximum size, in bytes, for a request", "64"},
            {"mem_size", "(string) Size of memory with units (SI ok). E.g., '2GiB'.", NULL},
            /* Own parameters */
            {"access_time", "(string) Constant latency of memory operations. With units (SI ok).", "100ns"} )

/* Begin class definition */
    SimpleMemory();
    SimpleMemory(Component *comp, Params &params);
    bool issueRequest(ReqId, Addr, bool, unsigned );
    virtual int32_t getMaxReqPerCycle() { return 1; }
    virtual bool isClocked() { return false; }    

public:
    class MemCtrlEvent : public SST::Event {
    public:
        MemCtrlEvent( ReqId id_) : SST::Event(), reqId(id_)
        { }

		ReqId reqId;
     
    private:   
        MemCtrlEvent() {} // For Serialization only
        
    public:
        void serialize_order(SST::Core::Serialization::serializer &ser)  override {
            Event::serialize_order(ser);
            ser & reqId;  // Cannot serialize pointers unless they are a serializable object
       }
        
        ImplementSerializable(SST::MemHierarchy::SimpleMemory::MemCtrlEvent);
    };

    void handleSelfEvent(SST::Event *event);

    Link *self_link;
};

}
}

#endif
