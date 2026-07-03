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

/*
 * Raw-libfabric end-to-end test of the FI_COLLECTIVE bridge (spike).
 *
 * This is a *client* of the sumi OFI provider -- it calls the standard
 * libfabric API directly (no MPI, no MVAPICH2). It proves the path
 *
 *     fi_allreduce -> sumi_ep_collective_ops.allreduce
 *                  -> CollectiveEngine::allreduce
 *                  -> CollectiveRegistry (allreduce_alg / SUMI_ALLREDUCE_ALG)
 *
 * end to end: every rank contributes (rank+1) and the result must equal the
 * known all-reduce sum n(n+1)/2. Flip SUMI_ALLREDUCE_ALG=ring vs recdouble
 * between runs: the result stays correct while the reported time changes --
 * that observably proves the registry is in the loop.
 *
 * Run:  sst test_fi_allreduce.py            (NRANKS=<n> to change rank count)
 */

#define ssthg_app_name fi_allreduce_test

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_collective.h>
#include <rdma/fi_errno.h>

#include <mercury/common/skeleton.h>

#define CHECK(call, what) do {                                   \
    int _r = (call);                                             \
    if (_r) { printf("FAIL: %s -> %d\n", what, _r); return 1; }  \
  } while (0)

int main(int argc, char** argv)
{
  struct fi_info* info = NULL;
  // NULL hints: the provider advertises FI_MSG/RMA/TAGGED/ATOMICS (not
  // FI_COLLECTIVE), so we must not request FI_COLLECTIVE in caps or getinfo
  // would filter us out. The collective ops are attached to the endpoint
  // regardless of the advertised caps (spike).
  CHECK(fi_getinfo(FI_VERSION(1, 9), NULL, NULL, 0, NULL, &info), "fi_getinfo");
  if (!info) { printf("FAIL: no fi_info\n"); return 1; }

  struct fid_fabric* fab = NULL;
  CHECK(fi_fabric(info->fabric_attr, &fab, NULL), "fi_fabric");

  struct fid_domain* dom = NULL;
  CHECK(fi_domain(fab, info, &dom, NULL), "fi_domain");

  struct fid_ep* ep = NULL;
  CHECK(fi_endpoint(dom, info, &ep, NULL), "fi_endpoint");

  struct fi_cq_attr cq_attr;
  memset(&cq_attr, 0, sizeof(cq_attr));
  cq_attr.format = FI_CQ_FORMAT_CONTEXT;
  cq_attr.size = 16;
  struct fid_cq* cq = NULL;
  CHECK(fi_cq_open(dom, &cq_attr, &cq, NULL), "fi_cq_open");

  CHECK(fi_ep_bind(ep, &cq->fid, FI_TRANSMIT | FI_RECV), "fi_ep_bind(cq)");
  CHECK(fi_enable(ep), "fi_enable");

  // Rank comes from the endpoint's own name. The default addr_format is
  // FI_ADDR_STR, encoded "<rank>.<cq>" (see sumi_getname), so atoi() peels off
  // the rank. Size is passed by the driver via NRANKS (matches its topology).
  char namebuf[64];
  size_t namelen = sizeof(namebuf);
  CHECK(fi_getname(&ep->fid, namebuf, &namelen), "fi_getname");
  int rank = atoi(namebuf);
  const char* nr = getenv("NRANKS");
  int size = nr ? atoi(nr) : 8;

  const int32_t sum_expected = size * (size + 1) / 2;  // sum of 1..size
  const int root = 0;

  // Drain exactly one collective completion and check its context. Every op
  // posts one completion to the send CQ (spike: blocking op + one completion).
  struct fi_cq_entry comp;
  void* ctx;
  #define DRAIN(what) do {                                              \
      memset(&comp, 0, sizeof(comp));                                   \
      ssize_t _g = fi_cq_sread(cq, &comp, 1, NULL, -1);                 \
      if (_g != 1) { printf("FAIL: %s cq_sread -> %zd\n", what, _g); return 1; } \
      if (comp.op_context != ctx) {                                     \
        printf("FAIL: %s completion ctx %p != %p\n", what,              \
               comp.op_context, ctx); return 1; }                       \
    } while (0)

  // --- barrier ---
  ctx = (void*) 0xBA0;
  if (fi_barrier(ep, FI_ADDR_UNSPEC, ctx)) { printf("FAIL: fi_barrier\n"); return 1; }
  DRAIN("barrier");

  // --- broadcast: root seeds a value, everyone must end up with it ---
  ctx = (void*) 0xBCA;
  int32_t bval = (rank == root) ? 0x1234 : -1;
  if (fi_broadcast(ep, &bval, 1, NULL, FI_ADDR_UNSPEC, (fi_addr_t) root,
                   FI_INT32, 0, ctx)) { printf("FAIL: fi_broadcast\n"); return 1; }
  DRAIN("broadcast");
  if (bval != 0x1234) {
    printf("FAIL: broadcast rank %d got %d expected %d\n", rank, bval, 0x1234);
    return 1;
  }

  // --- reduce (SUM to root): root must see n(n+1)/2 ---
  ctx = (void*) 0x4ED;
  int32_t rsrc = rank + 1, rdst = -1;
  if (fi_reduce(ep, &rsrc, 1, NULL, &rdst, NULL, FI_ADDR_UNSPEC,
                (fi_addr_t) root, FI_INT32, FI_SUM, 0, ctx)) {
    printf("FAIL: fi_reduce\n"); return 1;
  }
  DRAIN("reduce");
  if (rank == root && rdst != sum_expected) {
    printf("FAIL: reduce root got %d expected %d\n", rdst, sum_expected);
    return 1;
  }

  // --- allreduce (SUM): everyone must see n(n+1)/2; algorithm from registry ---
  ctx = (void*) 0xC0FFEE;
  int32_t src = rank + 1, result = -1;
  if (fi_allreduce(ep, &src, 1, NULL, &result, NULL, FI_ADDR_UNSPEC,
                   FI_INT32, FI_SUM, 0, ctx)) {
    printf("FAIL: fi_allreduce\n"); return 1;
  }
  DRAIN("allreduce");
  if (result != sum_expected) {
    printf("FAIL: allreduce rank %d got %d expected %d\n",
           rank, result, sum_expected);
    return 1;
  }

  // The vector collectives use size-element buffers; keep the test bounded.
  // N>=4: the engine's BtreeScatter has a pre-existing edge bug at N=2 (the
  // odd-ranked midpoint never copies its temp recv buffer into the result in
  // finalizeBuffers), unrelated to this provider bridge. barrier/broadcast/
  // reduce/allreduce above already cover the small-N cases.
  if (size >= 4 && size <= 64) {
    // --- allgather: result[i] must equal contribution of rank i (= i+1) ---
    ctx = (void*) 0xA11;
    int32_t agsrc = rank + 1, agdst[64];
    for (int i = 0; i < size; ++i) agdst[i] = -1;
    if (fi_allgather(ep, &agsrc, 1, NULL, agdst, NULL, FI_ADDR_UNSPEC,
                     FI_INT32, 0, ctx)) { printf("FAIL: fi_allgather\n"); return 1; }
    DRAIN("allgather");
    for (int i = 0; i < size; ++i) if (agdst[i] != i + 1) {
      printf("FAIL: allgather[%d]=%d expected %d\n", i, agdst[i], i + 1); return 1;
    }

    // --- gather to root: root's result[i] must equal i+1 ---
    ctx = (void*) 0x6A0;
    int32_t gsrc = rank + 1, gdst[64];
    for (int i = 0; i < size; ++i) gdst[i] = -1;
    if (fi_gather(ep, &gsrc, 1, NULL, gdst, NULL, FI_ADDR_UNSPEC, (fi_addr_t) root,
                  FI_INT32, 0, ctx)) { printf("FAIL: fi_gather\n"); return 1; }
    DRAIN("gather");
    if (rank == root) for (int i = 0; i < size; ++i) if (gdst[i] != i + 1) {
      printf("FAIL: gather[%d]=%d expected %d\n", i, gdst[i], i + 1); return 1;
    }

    // --- scatter from root: rank r must receive r+1 ---
    ctx = (void*) 0x5CA;
    int32_t ssrc[64], sdst = -1;
    if (rank == root) for (int i = 0; i < size; ++i) ssrc[i] = i + 1;
    if (fi_scatter(ep, ssrc, 1, NULL, &sdst, NULL, FI_ADDR_UNSPEC, (fi_addr_t) root,
                   FI_INT32, 0, ctx)) { printf("FAIL: fi_scatter\n"); return 1; }
    DRAIN("scatter");
    if (sdst != rank + 1) {
      printf("FAIL: scatter rank %d got %d expected %d\n", rank, sdst, rank + 1);
      return 1;
    }

    // NOTE: fi_reduce_scatter is wired in the provider (sumi_ep_reduce_scatter)
    // but NOT exercised here: the engine's HalvingReduceScatter DAG is a stub on
    // devel (aborts "halving_reduce_scatter: not implemented"). It will work
    // once the engine actor is implemented -- no provider change needed.

    // --- sub-communicator via fi_join_collective: even ranks only ---
    // Build an address set of the even global ranks, join it into a
    // sub-communicator, and allreduce over just those ranks. Proves coll_addr
    // now selects a real subgroup (not the world) through the engine + registry.
    struct fi_av_attr av_attr;
    memset(&av_attr, 0, sizeof(av_attr));
    av_attr.type = FI_AV_MAP;
    struct fid_av* av = NULL;
    CHECK(fi_av_open(dom, &av_attr, &av, NULL), "fi_av_open");

    struct fid_av_set* aset = NULL;
    CHECK(fi_av_set(av, NULL, &aset, NULL), "fi_av_set");

    int32_t sub_sum = 0;
    for (int r = 0; r < size; r += 2) {
      char a[32];
      snprintf(a, sizeof(a), "%010d.%05d", r, 0);
      fi_addr_t fa = FI_ADDR_UNSPEC;
      if (fi_av_insert(av, a, 1, &fa, 0, NULL) < 0) { printf("FAIL: fi_av_insert\n"); return 1; }
      CHECK(fi_av_set_insert(aset, fa), "fi_av_set_insert");
      sub_sum += (r + 1);
    }

    if (rank % 2 == 0) {   // only members join and collectively operate
      struct fid_mc* mc = NULL;
      ctx = (void*) 0x10;
      CHECK(fi_join_collective(ep, FI_ADDR_UNSPEC, aset, 0, &mc, ctx),
            "fi_join_collective");
      fi_addr_t coll = fi_mc_addr(mc);

      ctx = (void*) 0x11;
      int32_t ssub = rank + 1, rsub = -1;
      if (fi_allreduce(ep, &ssub, 1, NULL, &rsub, NULL, coll, FI_INT32, FI_SUM,
                       0, ctx)) { printf("FAIL: sub allreduce\n"); return 1; }
      DRAIN("sub-allreduce");
      if (rsub != sub_sum) {
        printf("FAIL: sub-allreduce (even) rank %d got %d expected %d\n",
               rank, rsub, sub_sum);
        return 1;
      }
      fi_close(&mc->fid);
    }
    fi_close(&aset->fid);
    fi_close(&av->fid);
  }

  if (rank == 0) {
    const char* alg = getenv("SUMI_ALLREDUCE_ALG");
    const char* vec = (size >= 4) ? ",allgather,gather,scatter,join+sub-allreduce" : "";
    printf("PASS: fi_collectives (%d ranks; barrier,broadcast,reduce,allreduce%s;"
           " SUM=%d, allreduce_alg=%s)\n",
           size, vec, result, alg ? alg : "default");
  }
  return 0;
}
