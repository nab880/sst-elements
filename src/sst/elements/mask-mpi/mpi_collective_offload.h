/**
Copyright 2009-2026 National Technology and Engineering Solutions of Sandia,
LLC (NTESS).  Under the terms of Contract DE-NA-0003525 with NTESS, the U.S.
Government retains certain rights in this software.

Copyright (c) 2009-2026, NTESS
All rights reserved.
*/

#pragma once

#include <mpi_request.h>
#include <mpi_types.h>

#include <merlin/services/collective/collectiveEndpoint.h>

#include <cstdint>

namespace SST::Hg {
class Thread;
}

namespace SST::MASKMPI {

class MpiApi;
class MpiComm;
class MpiRequest;

/**
 * Blocking allreduce offload for one MpiApi.
 *
 * Owns the binding to the NIC's collective endpoint, the single outstanding
 * native request, and the block/unblock handshake with the endpoint's
 * completion and ready callbacks.  Policy: a call the static profile cannot
 * carry (communicator, operation, datatype, count, noncontiguous datatype,
 * timing-only buffers) falls back to the software collective before any
 * packet is injected; a node that cannot host the endpoint at all while
 * offload is enabled is a configuration error and aborts, so an experiment
 * that asked for offload never silently measures software.
 */
class MpiCollectiveOffload final : private SST::Collective::CollectiveCompletionSink,
                                   private SST::Collective::CollectiveReadySink
{
 public:
  MpiCollectiveOffload(MpiApi& api, bool enabled);

  bool enabled() const { return enabled_; }

  /** Returns true when the call completed through the offload; false leaves op for the software path. */
  bool tryBlockingAllreduce(CollectiveOp::ptr& op, MPI_Datatype type, MPI_Op mop);

 private:
  bool bind();
  void clearRequest();
  void complete(uint64_t invocation_id,
                SST::Collective::CollectiveCompletionStatus status) override;
  void ready() override;

  MpiApi& api_;
  bool enabled_;
  SST::Collective::CollectiveEndpoint* endpoint_ = nullptr;
  MpiRequest* request_ = nullptr;
  MpiComm* request_comm_ = nullptr;
  SST::Hg::Thread* waiter_ = nullptr;
  bool waiting_blocked_ = false;
  int request_tag_ = 0;
  uint64_t request_invocation_ = 0;
  bool ready_ = false;
  uint64_t next_invocation_ = 1;
};

} // namespace SST::MASKMPI
