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

  // An event queue receives FI_JOIN_COMPLETE from fi_join_collective (below).
  struct fi_eq_attr eq_attr;
  memset(&eq_attr, 0, sizeof(eq_attr));
  eq_attr.size = 16;
  eq_attr.wait_obj = FI_WAIT_NONE;
  struct fid_eq* eq = NULL;
  CHECK(fi_eq_open(fab, &eq_attr, &eq, NULL), "fi_eq_open");
  CHECK(fi_ep_bind(ep, &eq->fid, 0), "fi_ep_bind(eq)");

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

  // --- two overlapping allreduces: issue A and B back-to-back WITHOUT draining
  // between them, then collect both completions. This only works because the ops
  // are non-blocking (each returns before completing) -- with the old blocking
  // model the first op would run to completion inside the call. Progress is
  // driven when we finally read the CQ. Completions may arrive in either order,
  // so match by context. ---
  {
    void* ctxA = (void*) 0xA1;
    void* ctxB = (void*) 0xB2;
    int32_t aSrc = rank + 1, aDst = -1;
    int32_t bSrc = 2 * (rank + 1), bDst = -1;
    if (fi_allreduce(ep, &aSrc, 1, NULL, &aDst, NULL, FI_ADDR_UNSPEC,
                     FI_INT32, FI_SUM, 0, ctxA)) { printf("FAIL: overlap A\n"); return 1; }
    if (fi_allreduce(ep, &bSrc, 1, NULL, &bDst, NULL, FI_ADDR_UNSPEC,
                     FI_INT32, FI_SUM, 0, ctxB)) { printf("FAIL: overlap B\n"); return 1; }
    int gotA = 0, gotB = 0;
    for (int k = 0; k < 2; ++k) {
      struct fi_cq_entry e;
      ssize_t g = fi_cq_sread(cq, &e, 1, NULL, -1);
      if (g != 1) { printf("FAIL: overlap cq_sread -> %zd\n", g); return 1; }
      if (e.op_context == ctxA) gotA = 1;
      else if (e.op_context == ctxB) gotB = 1;
      else { printf("FAIL: overlap unexpected ctx %p\n", e.op_context); return 1; }
    }
    if (!gotA || !gotB) { printf("FAIL: overlap missing completion A=%d B=%d\n", gotA, gotB); return 1; }
    if (aDst != sum_expected || bDst != 2 * sum_expected) {
      printf("FAIL: overlap rank %d A=%d (exp %d) B=%d (exp %d)\n",
             rank, aDst, sum_expected, bDst, 2 * sum_expected);
      return 1;
    }
  }

  // --- allreduce FI_BOR over FI_INT64: exercise a bitwise op on a wide integer
  // type (not the FI_SUM/FI_INT32 default path). Rank r contributes bit r%60;
  // the OR is the set of all contributed bits. ---
  ctx = (void*) 0xB0;
  int64_t borsrc = (int64_t)1 << (rank % 60), bordst = -1;
  int64_t bor_expected = 0;
  for (int r = 0; r < size; ++r) bor_expected |= (int64_t)1 << (r % 60);
  if (fi_allreduce(ep, &borsrc, 1, NULL, &bordst, NULL, FI_ADDR_UNSPEC,
                   FI_INT64, FI_BOR, 0, ctx)) { printf("FAIL: fi_allreduce BOR\n"); return 1; }
  DRAIN("allreduce-bor");
  if (bordst != bor_expected) {
    printf("FAIL: allreduce BOR rank %d got %lld expected %lld\n",
           rank, (long long)bordst, (long long)bor_expected);
    return 1;
  }

  // --- allreduce FI_MAX over FI_DOUBLE: exercise a floating datatype. Rank r
  // contributes (r+1)*0.5; the max is size*0.5. ---
  ctx = (void*) 0xDA0;
  double maxsrc = (rank + 1) * 0.5, maxdst = -1;
  if (fi_allreduce(ep, &maxsrc, 1, NULL, &maxdst, NULL, FI_ADDR_UNSPEC,
                   FI_DOUBLE, FI_MAX, 0, ctx)) { printf("FAIL: fi_allreduce MAX\n"); return 1; }
  DRAIN("allreduce-max");
  if (maxdst != size * 0.5) {
    printf("FAIL: allreduce MAX rank %d got %f expected %f\n",
           rank, maxdst, size * 0.5);
    return 1;
  }

  // The vector collectives use size-element buffers; keep the test bounded.
  if (size >= 2 && size <= 64) {
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

    // --- alltoall: rank r sends element j=(r+1)*100+(j+1) to rank j, so
    // rank r receives from rank i the value (i+1)*100+(r+1). Distinct per
    // (src,dst) pair, so it catches any block transposition. Exercises the
    // engine's Bruck/Direct all-to-all actor end to end. ---
    ctx = (void*) 0xA27A;
    int32_t a2asrc[64], a2adst[64];
    for (int j = 0; j < size; ++j) { a2asrc[j] = (rank + 1) * 100 + (j + 1); a2adst[j] = -1; }
    if (fi_alltoall(ep, a2asrc, 1, NULL, a2adst, NULL, FI_ADDR_UNSPEC,
                    FI_INT32, 0, ctx)) { printf("FAIL: fi_alltoall\n"); return 1; }
    DRAIN("alltoall");
    for (int i = 0; i < size; ++i) if (a2adst[i] != (i + 1) * 100 + (rank + 1)) {
      printf("FAIL: alltoall rank %d [%d]=%d expected %d\n",
             rank, i, a2adst[i], (i + 1) * 100 + (rank + 1)); return 1;
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

    // --- reduce_scatter (SUM): each rank r must receive its own block r,
    // reduced across every rank. Rank r contributes block j = (r+1)*(j+1), so
    // block j reduces to (j+1)*sum(r+1) = (j+1)*n(n+1)/2 and rank r keeps that
    // for j==r. Exercises the engine's HalvingReduceScatter DAG (recursive
    // halving for power-of-two n, ring otherwise) end to end. count=1 element
    // per rank; the input buffer holds one element per destination rank.
    ctx = (void*) 0x25CA;
    int32_t rssrc[64], rsdst = -1;
    for (int j = 0; j < size; ++j) rssrc[j] = (rank + 1) * (j + 1);
    if (fi_reduce_scatter(ep, rssrc, 1, NULL, &rsdst, NULL, FI_ADDR_UNSPEC,
                          FI_INT32, FI_SUM, 0, ctx)) {
      printf("FAIL: fi_reduce_scatter\n"); return 1;
    }
    DRAIN("reduce_scatter");
    int32_t rs_expected = (rank + 1) * sum_expected;
    if (rsdst != rs_expected) {
      printf("FAIL: reduce_scatter rank %d got %d expected %d\n",
             rank, rsdst, rs_expected);
      return 1;
    }

    // --- sub-communicator via fi_join_collective: even ranks only (>=2) ---
    if (size >= 4) {
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
      void* jctx = (void*) 0x10;
      CHECK(fi_join_collective(ep, FI_ADDR_UNSPEC, aset, 0, &mc, jctx),
            "fi_join_collective");

      // Async join contract: wait for FI_JOIN_COMPLETE on the EQ before using
      // the mc. The event echoes the mc fid and the join context.
      struct fi_eq_entry je;
      uint32_t jev = 0;
      ssize_t jr = fi_eq_sread(eq, &jev, &je, sizeof(je), -1, 0);
      if (jr < (ssize_t) sizeof(je) || jev != FI_JOIN_COMPLETE ||
          je.fid != &mc->fid || je.context != jctx) {
        printf("FAIL: join eq event jr=%zd ev=%u fid=%p ctx=%p\n",
               jr, jev, (void*) je.fid, je.context);
        return 1;
      }
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

    // --- AV-set algebra + range form: build the even set via the range form
    // {start=0,stride=2,count=n/2}, and the full set via {start=0,stride=1,
    // count=n}. intersect(full, even) must yield the even subgroup; diff(full,
    // even) the odd subgroup. Each rank joins its own subgroup (built purely by
    // the algebra) and sub-allreduces to prove the resulting membership. ---
    {
    struct fi_av_set_attr even_attr;
    memset(&even_attr, 0, sizeof(even_attr));
    even_attr.start_addr = 0; even_attr.stride = 2; even_attr.count = (size + 1) / 2;

    struct fi_av_set_attr all_attr;
    memset(&all_attr, 0, sizeof(all_attr));
    all_attr.start_addr = 0; all_attr.stride = 1; all_attr.count = size;

    int32_t even_sum = 0, odd_sum = 0;
    for (int r = 0; r < size; ++r) ((r % 2) ? odd_sum : even_sum) += (r + 1);

    struct fid_av_set* evenSet = NULL;
    CHECK(fi_av_set(av, &even_attr, &evenSet, NULL), "fi_av_set(even range)");
    struct fid_av_set* mySet = NULL;
    CHECK(fi_av_set(av, &all_attr, &mySet, NULL), "fi_av_set(all range)");

    int32_t want;
    if (rank % 2 == 0) {                 // even ranks: full ∩ even = even
      CHECK(fi_av_set_intersect(mySet, evenSet), "fi_av_set_intersect");
      want = even_sum;
    } else {                             // odd ranks: full \ even = odd
      CHECK(fi_av_set_diff(mySet, evenSet), "fi_av_set_diff");
      want = odd_sum;
    }

    struct fid_mc* mc2 = NULL;
    void* jctx2 = (void*) 0x20;
    CHECK(fi_join_collective(ep, FI_ADDR_UNSPEC, mySet, 0, &mc2, jctx2),
          "fi_join_collective(algebra)");
    struct fi_eq_entry je2;
    uint32_t jev2 = 0;
    ssize_t jr2 = fi_eq_sread(eq, &jev2, &je2, sizeof(je2), -1, 0);
    if (jr2 < (ssize_t) sizeof(je2) || jev2 != FI_JOIN_COMPLETE || je2.fid != &mc2->fid) {
      printf("FAIL: algebra join eq event jr=%zd ev=%u\n", jr2, jev2); return 1;
    }
    ctx = (void*) 0x21;
    int32_t asub = rank + 1, arsub = -1;
    if (fi_allreduce(ep, &asub, 1, NULL, &arsub, NULL, fi_mc_addr(mc2),
                     FI_INT32, FI_SUM, 0, ctx)) { printf("FAIL: algebra allreduce\n"); return 1; }
    DRAIN("algebra-sub-allreduce");
    if (arsub != want) {
      printf("FAIL: algebra sub-allreduce rank %d got %d expected %d\n",
             rank, arsub, want);
      return 1;
    }
    fi_close(&mc2->fid);
    fi_close(&mySet->fid);
    fi_close(&evenSet->fid);
    }

    fi_close(&av->fid);
    } // if (size >= 4): join
  } // if (size >= 2): vector collectives

  if (rank == 0) {
    const char* alg = getenv("SUMI_ALLREDUCE_ALG");
    const char* vec = (size >= 2) ? ",allgather,alltoall,gather,scatter,reduce_scatter" : "";
    const char* jn  = (size >= 4) ? ",join(eq)+sub-allreduce,avset-range+intersect/diff" : "";
    printf("PASS: fi_collectives (%d ranks; barrier,broadcast,reduce,allreduce[sum/bor/max],overlap2%s%s;"
           " SUM=%d, allreduce_alg=%s)\n",
           size, vec, jn, result, alg ? alg : "default");
  }
  return 0;
}
