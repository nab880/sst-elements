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
#include <iris/sumi/null_buffer.h>
//#include <sprockit/output.h>
#include <mercury/common/stl_string.h>
#include <cstring>

#define divide_by_2_round_up(x) ((x/2) + (x%2))

#define divide_by_2_round_down(x) (x/2)

//using namespace sprockit::dbg;

namespace SST::Iris::sumi {

static bool isPowerOf2(int x){
  return x > 0 && (x & (x-1)) == 0;
}

void
HalvingReduceScatterActor::initBuffers()
{
  void* dst = result_buffer_;   //caller's per-rank result (nelems_ elements)
  void* src = send_buffer_;     //full contribution array (nelems_ * dom_nproc_ elements)

  slicer_->fxn = fxn_;

  final_dst_ = dst;

  uint64_t total_size = static_cast<uint64_t>(nelems_) * dom_nproc_ * type_size_;

  //we reduce the whole array in place in a private scratch buffer, then hand
  //the caller only its own block in finalize()
  reduce_buffer_ = my_api_->allocateWorkspace(total_size, src);
  my_api_->memcopy(reduce_buffer_, src, total_size);
  result_buffer_ = reduce_buffer_;

  //incoming reduce data lands here (indexed by the same element offset as the
  //result buffer), then is folded into result_buffer_ by RecvAction::reduce
  recv_buffer_ = my_api_->allocateWorkspace(total_size, src);

  //SendAction::in_place reads straight out of result_buffer_
  send_buffer_ = result_buffer_;
}

void
HalvingReduceScatterActor::finalize()
{
  //scatter step: keep only my block (block dom_me_) in the caller's buffer
  if (isNonNullBuffer(final_dst_) && isNonNullBuffer(reduce_buffer_)){
    uint64_t block_bytes = static_cast<uint64_t>(nelems_) * type_size_;
    void* my_block = static_cast<char*>(reduce_buffer_) +
                     static_cast<uint64_t>(dom_me_) * block_bytes;
    my_api_->memcopy(final_dst_, my_block, block_bytes);
  }
  //report the caller's buffer as the result, not the scratch we free below
  result_buffer_ = final_dst_;
}

void
HalvingReduceScatterActor::finalizeBuffers()
{
  uint64_t total_size = static_cast<uint64_t>(nelems_) * dom_nproc_ * type_size_;
  if (reduce_buffer_){
    my_api_->freeWorkspace(reduce_buffer_, total_size);
    reduce_buffer_ = nullptr;
  }
  if (recv_buffer_){
    my_api_->freeWorkspace(recv_buffer_, total_size);
    recv_buffer_ = nullptr;
  }
}

void
HalvingReduceScatterActor::initDag()
{
  //Recursive halving needs a power-of-two rank count. For anything else we fall
  //back to a ring reduce-scatter, which is correct for any number of ranks.
  //Both leave rank r holding the fully reduced block r (natural MPI ordering),
  //so no post-permutation is required.
  if (isPowerOf2(dom_nproc_)){
    initHalvingDag();
  } else {
    initRingDag();
  }
}

void
HalvingReduceScatterActor::initHalvingDag()
{
  //Recursive-halving reduce-scatter, high bit first. Each round the still-active
  //contiguous block range is halved: I keep the half whose index bit matches my
  //rank bit and exchange the other half with partner = me ^ mask, reducing what
  //I receive into my kept half. After log2(nproc) rounds my range has narrowed
  //to exactly block dom_me_. Sizing the split by the current rank bit (rather
  //than low bit first) is what yields natural block ordering with no shuffle.
  int nproc = dom_nproc_;
  int me = dom_me_;

  //block range currently owned, in block units [lo, hi)
  int lo = 0, hi = nproc;
  Action *prev_send = nullptr, *prev_recv = nullptr;
  int round = 0;
  for (int mask = nproc >> 1; mask > 0; mask >>= 1, ++round){
    int partner = me ^ mask;
    int mid = (lo + hi) / 2;

    int send_lo, send_hi, keep_lo, keep_hi;
    if (me & mask){
      //keep the upper half, ship the lower half
      keep_lo = mid; keep_hi = hi;
      send_lo = lo;  send_hi = mid;
    } else {
      //keep the lower half, ship the upper half
      keep_lo = lo;  keep_hi = mid;
      send_lo = mid; send_hi = hi;
    }

    Action* send_ac = new SendAction(round, partner, SendAction::in_place);
    send_ac->offset = send_lo * nelems_;
    send_ac->nelems = (send_hi - send_lo) * nelems_;

    Action* recv_ac = new RecvAction(round, partner, RecvAction::reduce);
    recv_ac->offset = keep_lo * nelems_;
    recv_ac->nelems = (keep_hi - keep_lo) * nelems_;

    addDependency(prev_send, send_ac);
    addDependency(prev_send, recv_ac);
    addDependency(prev_recv, send_ac);
    addDependency(prev_recv, recv_ac);

    prev_send = send_ac;
    prev_recv = recv_ac;

    lo = keep_lo; hi = keep_hi;
  }
}

void
HalvingReduceScatterActor::initRingDag()
{
  //Ring reduce-scatter for non-power-of-two rank counts. Over nproc-1 steps each
  //rank forwards the block it accumulated last step to its right neighbor while
  //reducing the block arriving from its left neighbor. Indices are chosen so the
  //block that finishes accumulating on rank r is exactly block r.
  int nproc = dom_nproc_;
  int me = dom_me_;
  if (nproc > static_cast<int>(Action::max_round)){
    sst_hg_abort_printf("ring reduce-scatter supports at most %u ranks; "
                        "got %d", Action::max_round, nproc);
  }
  int right = (me + 1) % nproc;
  int left = (me + nproc - 1) % nproc;

  Action *prev_send = nullptr, *prev_recv = nullptr;
  for (int s = 1; s < nproc; ++s){
    int send_block = ((me - s) % nproc + nproc) % nproc;
    int recv_block = ((me - s - 1) % nproc + nproc) % nproc;

    Action* send_ac = new SendAction(s, right, SendAction::in_place);
    send_ac->offset = send_block * nelems_;
    send_ac->nelems = nelems_;

    Action* recv_ac = new RecvAction(s, left, RecvAction::reduce);
    recv_ac->offset = recv_block * nelems_;
    recv_ac->nelems = nelems_;

    //I can only forward a block once I've folded in the contribution I received
    //for it on the previous step, so serialize the ring round-by-round
    addDependency(prev_send, send_ac);
    addDependency(prev_send, recv_ac);
    addDependency(prev_recv, send_ac);
    addDependency(prev_recv, recv_ac);

    prev_send = send_ac;
    prev_recv = recv_ac;
  }
}

void
HalvingReduceScatterActor::bufferAction(void *dst_buffer, void *msg_buffer, Action* /*ac*/)
{
  //reductions are applied through the slicer on RecvAction::reduce; this is only
  //reached for any non-reducing copy actions in the DAG
  my_api_->memcopy(dst_buffer, msg_buffer, nelems_ * type_size_);
}

}
