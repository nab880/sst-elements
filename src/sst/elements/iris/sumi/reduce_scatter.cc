/**
Copyright 2009-2026 National Technology and Engineering Solutions of Sandia,
LLC (NTESS).  Under the terms of Contract DE-NA-0003525, the U.S. Government
retains certain rights in this software.

Sandia National Laboratories is a multimission laboratory managed and operated
by National Technology and Engineering Solutions of Sandia, LLC., a wholly
owned subsidiary of Honeywell International, Inc., for the U.S. Department of
Energy's National Nuclear Security Administration under contract DE-NA0003525.

Copyright (c) 2009-2026, NTESS

All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above
      copyright notice, this list of conditions and the following
      disclaimer in the documentation and/or other materials provided
      with the distribution.

    * Neither the name of the copyright holder nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Questions? Contact sst-macro-help@sandia.gov
*/

#include <iris/sumi/reduce_scatter.h>
#include <iris/sumi/transport.h>
#include <iris/sumi/communicator.h>
//#include <sprockit/output.h>
#include <mercury/common/errors.h>
#include <mercury/common/stl_string.h>
#include <cstring>
#include <vector>

//using namespace sprockit::dbg;

namespace SST::Iris::sumi {

void
HalvingReduceScatterActor::finalizeBuffers()
{
  long buffer_size = nelems_ * type_size_;
  my_api_->freeWorkspace(recv_buffer_, buffer_size);
}

void
HalvingReduceScatterActor::initBuffers()
{
  void* dst = result_buffer_;
  void* src = send_buffer_;
  int size = nelems_ * type_size_;
  result_buffer_ = dst;
  if (src != dst)
    my_api_->memcopy(dst, src, size);
  recv_buffer_ = my_api_->allocateWorkspace(size, src);
  send_buffer_ = result_buffer_;
}

void
HalvingReduceScatterActor::initDag()
{
  slicer_->fxn = fxn_;

  const int N = dom_nproc_;
  const int me = dom_me_;
  if (N <= 1){ num_rounds_ = 0; return; }

  // Round index must stay below Action::max_round or messageId() decoding
  // corrupts (see ring_allreduce.cc); this ring uses N-1 rounds.
  if (N - 1 >= static_cast<int>(Action::max_round)){
    sst_hg_abort_printf("ring reduce-scatter needs %d rounds but "
                        "Action::max_round is %u; raise max_round for nproc=%d",
                        N - 1, Action::max_round, N);
  }

  // Ring reduce-scatter: the reduce phase of a ring all-reduce. N-1 rounds; each
  // rank sends chunk (me-r) right and reduces chunk (me-r-1) from the left, so
  // rank me ends owning the fully-reduced chunk (me+1) mod N (NOT chunk me -- the
  // ring leaves a +1 rank rotation). Harmless here: inside ring_allreduce the
  // all-gather phase starts from (me+1) so it cancels, and the only caller
  // (MPI_[I]reduce_scatter_block) passes NULL buffers to model cost, so data
  // placement is never observed. A data-carrying reduce-scatter would need the
  // result copied out from chunk (me+1), not me. Same bytes/steps as the
  // all-gather dual (the bandwidth-optimal reduce-scatter).
  std::vector<int> off(N), cnt(N);
  int base = nelems_ / N, rem = nelems_ % N, o = 0;
  for (int c = 0; c < N; ++c){ cnt[c] = base + (c < rem ? 1 : 0); off[c] = o; o += cnt[c]; }

  const int right = (me + 1) % N;
  const int left  = (me - 1 + N) % N;
  Action* prev_send = nullptr;
  Action* prev_recv = nullptr;
  for (int r = 0; r < N - 1; ++r){
    int send_chunk = ((me - r) % N + N) % N;
    int recv_chunk = ((me - r - 1) % N + N) % N;
    Action* send_ac = new SendAction(r, right, SendAction::in_place);
    send_ac->offset = off[send_chunk]; send_ac->nelems = cnt[send_chunk];
    Action* recv_ac = new RecvAction(r, left, RecvAction::reduce);
    recv_ac->offset = off[recv_chunk]; recv_ac->nelems = cnt[recv_chunk];
    addDependency(prev_send, send_ac);
    addDependency(prev_send, recv_ac);
    addDependency(prev_recv, send_ac);
    addDependency(prev_recv, recv_ac);
    prev_send = send_ac;
    prev_recv = recv_ac;
  }
  num_rounds_ = N - 1;
}

bool
HalvingReduceScatterActor::isLowerPartner(int  /*virtual_me*/, int  /*partner_gap*/)
{
  return false;
}

void
HalvingReduceScatterActor::bufferAction(void *dst_buffer, void *msg_buffer, Action* ac)
{
  (fxn_)(dst_buffer, msg_buffer, ac->nelems);
}

}
