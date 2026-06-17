// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.
//
// Portions are copyright of other developers:
// See the file CONTRIBUTORS.TXT in the top level directory
// of the distribution for more information.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#pragma once

#include <iris/sumi/collective.h>
#include <iris/sumi/collective_actor.h>
#include <iris/sumi/collective_message.h>
#include <iris/sumi/comm_functions.h>

namespace SST::Iris::sumi {

// Ring all-reduce (WS2-2a): reduce-scatter + all-gather, 2(nproc-1) steps of
// nelems/nproc each, all traffic to the nearest neighbours (rank +/- 1). This is
// the bandwidth-optimal counterpart to the latency-optimal recursive-doubling
// WilkeHalvingAllreduce: more steps, but every message is small and local, so
// under a real fabric the two cross over with scale and message size. Selectable
// at runtime so a study can compare them on the same packet-level network.
class RingAllreduceActor :
  public DagCollectiveActor
{
 public:
  RingAllreduceActor(CollectiveEngine* engine, void* dst, void* src,
                     int nelems, int type_size, int tag, reduce_fxn fxn,
                     int cq_id, Communicator* comm) :
    DagCollectiveActor(Collective::allreduce, engine, dst, src, type_size, tag, cq_id, comm, fxn),
    fxn_(fxn), nelems_(nelems), num_reducing_rounds_(0), num_total_rounds_(0)
  {
  }

  std::string toString() const override {
    return "ring all reduce actor";
  }

  void bufferAction(void *dst_buffer, void *msg_buffer, Action* ac) override;

  Output output;

 private:
  void finalizeBuffers() override;
  void initBuffers() override;
  void initDag() override;

 private:
  reduce_fxn fxn_;
  int nelems_;
  int num_reducing_rounds_;
  int num_total_rounds_;
};

class RingAllreduce :
  public DagCollective
{
 public:
  RingAllreduce(CollectiveEngine* engine, void* dst, void* src,
                int nelems, int type_size, int tag, reduce_fxn fxn,
                int cq_id, Communicator* comm)
    : DagCollective(allreduce, engine, dst, src, type_size, tag, cq_id, comm),
      fxn_(fxn), nelems_(nelems)
  {
  }

  std::string toString() const override {
    return "sumi ring allreduce";
  }

  DagCollectiveActor* newActor() const override {
    return new RingAllreduceActor(engine_, dst_buffer_, src_buffer_,
                                  nelems_, type_size_, tag_, fxn_, cq_id_, comm_);
  }

 private:
  reduce_fxn fxn_;
  int nelems_;
};

}
