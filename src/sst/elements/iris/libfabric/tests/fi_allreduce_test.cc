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
 * Raw-libfabric end-to-end test of the FI_COLLECTIVE bridge.
 *
 * This is a *client* of the sumi OFI provider -- it calls the standard
 * libfabric API directly (no MPI, no MVAPICH2). It proves the path
 *
 *     fi_allreduce -> sumi_ep_collective_ops.allreduce
 *                  -> CollectiveEngine::allreduce
 *                  -> CollectiveRegistry (collective.allreduce / environment)
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
#include <limits.h>

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_collective.h>
#include <rdma/fi_errno.h>

#include <mercury/components/operating_system.h>
#include <mercury/common/skeleton.h>

#define CHECK(call, what) do {                                   \
    int _r = (call);                                             \
    if (_r) { printf("FAIL: %s -> %d\n", what, _r); return 1; }  \
  } while (0)

int main(int argc, char** argv)
{
  struct fi_info* info = NULL;
  struct fi_info* hints = fi_allocinfo();
  if (!hints) { printf("FAIL: fi_allocinfo\n"); return 1; }
  hints->caps = FI_COLLECTIVE;
  hints->addr_format = FI_ADDR_STR;
  CHECK(fi_getinfo(FI_VERSION(1, 9), NULL, NULL, 0, hints, &info), "fi_getinfo");
  fi_freeinfo(hints);
  if (!info) { printf("FAIL: no fi_info\n"); return 1; }

  struct fid_fabric* fab = NULL;
  CHECK(fi_fabric(info->fabric_attr, &fab, NULL), "fi_fabric");

  struct fid_domain* dom = NULL;
  CHECK(fi_domain(fab, info, &dom, NULL), "fi_domain");

  struct fid_ep* ep = NULL;
  CHECK(fi_endpoint(dom, info, &ep, NULL), "fi_endpoint");

  struct fi_cq_attr cq_attr;
  memset(&cq_attr, 0, sizeof(cq_attr));
  cq_attr.format = FI_CQ_FORMAT_MSG;
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

  // Skew provider CQ allocation history on one rank. Collective work routing
  // must not depend on locally allocated CQ numbers agreeing across ranks.
  struct fid_cq* skew_cq = NULL;
  if (rank == 0)
    CHECK(fi_cq_open(dom, &cq_attr, &skew_cq, NULL), "fi_cq_open(skew)");

  struct fi_collective_attr coll_attr;
  memset(&coll_attr, 0, sizeof(coll_attr));
  coll_attr.op = FI_SUM;
  coll_attr.datatype = FI_INT32;
  CHECK(fi_query_collective(dom, FI_ALLREDUCE, &coll_attr, 0),
        "fi_query_collective(allreduce)");
  if (coll_attr.max_members < (size_t) size) {
    printf("FAIL: collective max_members %zu < %d\n", coll_attr.max_members, size);
    return 1;
  }
  if (coll_attr.datatype_attr.count != (size_t) INT_MAX / sizeof(int32_t)) {
    printf("FAIL: allreduce max count %zu\n", coll_attr.datatype_attr.count);
    return 1;
  }
  struct fi_collective_attr allgather_attr;
  memset(&allgather_attr, 0, sizeof(allgather_attr));
  allgather_attr.datatype = FI_INT32;
  CHECK(fi_query_collective(dom, FI_ALLGATHER, &allgather_attr, 0),
        "fi_query_collective(allgather)");
  if (allgather_attr.datatype_attr.count !=
      (size_t) INT_MAX / sizeof(int32_t) / (size_t) size) {
    printf("FAIL: allgather max count %zu\n",
           allgather_attr.datatype_attr.count);
    return 1;
  }
  ssize_t query_rs = fi_query_collective(dom, FI_REDUCE_SCATTER, &coll_attr, 0);
  if (query_rs != -FI_EOPNOTSUPP) {
    printf("FAIL: query reduce-scatter returned %zd expected %d\n",
           query_rs, -FI_EOPNOTSUPP);
    return 1;
  }

  struct fi_av_attr av_attr;
  memset(&av_attr, 0, sizeof(av_attr));
  av_attr.type = FI_AV_TABLE;
  av_attr.count = size;
  struct fid_av* av = NULL;
  CHECK(fi_av_open(dom, &av_attr, &av, NULL), "fi_av_open");
  CHECK(fi_ep_bind(ep, &av->fid, 0), "fi_ep_bind(av)");

  enum { ADDR_LEN = 17 };
  char* world_names = (char*) calloc((size_t) size, ADDR_LEN);
  fi_addr_t* world = (fi_addr_t*) calloc((size_t) size, sizeof(*world));
  if (!world_names || !world) { printf("FAIL: world allocation\n"); return 1; }
  for (int i = 0; i < size; ++i)
    snprintf(world_names + i * ADDR_LEN, ADDR_LEN, "%010d.%05d", i, 0);
  int inserted = fi_av_insert(av, world_names, (size_t) size, world, 0, NULL);
  if (inserted != size) {
    printf("FAIL: fi_av_insert -> %d expected %d\n", inserted, size);
    return 1;
  }
  char alternate_root_name[ADDR_LEN];
  snprintf(alternate_root_name, sizeof(alternate_root_name),
           "%010d.%05d", 0, 1);
  fi_addr_t alternate_root = FI_ADDR_NOTAVAIL;
  inserted = fi_av_insert(av, alternate_root_name, 1, &alternate_root, 0, NULL);
  if (inserted != 1) {
    printf("FAIL: alternate-root fi_av_insert -> %d\n", inserted);
    return 1;
  }

  struct fi_av_set_attr set_attr;
  memset(&set_attr, 0, sizeof(set_attr));
  set_attr.count = (size_t) size;
  set_attr.start_addr = world[0];
  set_attr.end_addr = world[size - 1];
  set_attr.stride = 1;
  struct fid_av_set* world_set = NULL;
  CHECK(fi_av_set(av, &set_attr, &world_set, NULL), "fi_av_set");

  struct fi_av_set_attr empty_attr;
  memset(&empty_attr, 0, sizeof(empty_attr));
  empty_attr.start_addr = FI_ADDR_NOTAVAIL;
  empty_attr.end_addr = FI_ADDR_NOTAVAIL;
  struct fid_av_set* empty_set = NULL;
  CHECK(fi_av_set(av, &empty_attr, &empty_set, NULL), "fi_av_set(empty)");
  CHECK(fi_av_set_insert(empty_set, world[rank]), "fi_av_set_insert(empty)");
  CHECK(fi_av_set_remove(empty_set, world[rank]), "fi_av_set_remove(empty)");
  CHECK(fi_close(&empty_set->fid), "fi_close(empty_set)");

  struct fi_eq_attr eq_attr;
  memset(&eq_attr, 0, sizeof(eq_attr));
  eq_attr.wait_obj = FI_WAIT_NONE;
  eq_attr.size = 4;
  struct fid_eq* eq = NULL;
  CHECK(fi_eq_open(fab, &eq_attr, &eq, NULL), "fi_eq_open");
  CHECK(fi_ep_bind(ep, &eq->fid, 0), "fi_ep_bind(eq)");

  struct fi_eq_entry timeout_entry;
  uint32_t timeout_event = 0;
  double timeout_start = SST::Hg::OperatingSystem::currentOs()->now().sec();
  ssize_t timeout_read = fi_eq_sread(eq, &timeout_event, &timeout_entry,
                                     sizeof(timeout_entry), 1, 0);
  double timeout_elapsed =
      SST::Hg::OperatingSystem::currentOs()->now().sec() - timeout_start;
  if (timeout_read != -FI_EAGAIN || timeout_elapsed < 0.0009) {
    printf("FAIL: simulated EQ timeout read=%zd elapsed=%g\n",
           timeout_read, timeout_elapsed);
    return 1;
  }

  struct fid_mc* mc = NULL;
  void* join_ctx = (void*) 0xC011EC7;
  CHECK(fi_join_collective(ep, FI_ADDR_NOTAVAIL, world_set, 0, &mc, join_ctx),
        "fi_join_collective");
  struct fi_eq_entry join_entry;
  uint32_t event = 0;
  ssize_t eq_read = fi_eq_sread(eq, &event, &join_entry, sizeof(join_entry), -1, 0);
  if (eq_read != (ssize_t) sizeof(join_entry) || event != FI_JOIN_COMPLETE ||
      join_entry.fid != &mc->fid || join_entry.context != join_ctx) {
    printf("FAIL: join completion read=%zd event=%u fid=%p ctx=%p\n",
           eq_read, event, (void*) join_entry.fid, join_entry.context);
    return 1;
  }
  fi_addr_t coll_addr = mc->fi_addr;

  struct fid_av_set* duplicate_set = NULL;
  CHECK(fi_av_set(av, &empty_attr, &duplicate_set, NULL),
        "fi_av_set(duplicate rank)");
  CHECK(fi_av_set_insert(duplicate_set, world[0]),
        "fi_av_set_insert(duplicate rank primary)");
  CHECK(fi_av_set_insert(duplicate_set, alternate_root),
        "fi_av_set_insert(duplicate rank alternate)");
  struct fid_mc* duplicate_mc = NULL;
  ssize_t duplicate_join = fi_join_collective(
      ep, FI_ADDR_NOTAVAIL, duplicate_set, 0, &duplicate_mc, NULL);
  if (duplicate_join != -FI_EINVAL) {
    printf("FAIL: duplicate-rank join returned %zd expected %d\n",
           duplicate_join, -FI_EINVAL);
    return 1;
  }
  CHECK(fi_close(&duplicate_set->fid), "fi_close(duplicate_set)");

  const int32_t sum_expected = size * (size + 1) / 2;  // sum of 1..size
  const int root = 0;

  // Drain exactly one collective completion and check its context. Every op
  // posts one completion to the send CQ (blocking op + one completion).
  struct fi_cq_msg_entry comp;
  void* ctx;
  #define DRAIN(what, roles) do {                                       \
      memset(&comp, 0, sizeof(comp));                                   \
      ssize_t _g = fi_cq_sread(cq, &comp, 1, NULL, -1);                 \
      if (_g != 1) { printf("FAIL: %s cq_sread -> %zd\n", what, _g); return 1; } \
      if (comp.op_context != ctx) {                                     \
        printf("FAIL: %s completion ctx %p != %p\n", what,              \
               comp.op_context, ctx); return 1; }                       \
      if (comp.flags != (FI_COLLECTIVE | (roles))) {                    \
        printf("FAIL: %s completion flags 0x%llx\n", what,              \
               (unsigned long long) comp.flags); return 1; }            \
    } while (0)

  int32_t invalid_root_value = -1;
  ssize_t invalid_root_ret = fi_broadcast(
      ep, &invalid_root_value, 1, NULL, coll_addr, alternate_root,
      FI_INT32, FI_RECV, NULL);
  if (invalid_root_ret != -FI_EINVAL) {
    printf("FAIL: out-of-group root returned %zd expected %d\n",
           invalid_root_ret, -FI_EINVAL);
    return 1;
  }

  // --- barrier ---
  ctx = (void*) 0xBA0;
  if (fi_barrier(ep, coll_addr, ctx)) { printf("FAIL: fi_barrier\n"); return 1; }
  DRAIN("barrier", FI_SEND);

  // --- broadcast: root seeds a value, everyone must end up with it ---
  ctx = (void*) 0xBCA;
  int32_t bval = (rank == root) ? 0x1234 : -1;
  ssize_t bad_bcast = fi_broadcast(ep, &bval, 1, NULL, coll_addr,
                                   world[root], FI_INT32, 0, ctx);
  if (bad_bcast != -FI_EINVAL) {
    printf("FAIL: flagless broadcast returned %zd expected %d\n",
           bad_bcast, -FI_EINVAL);
    return 1;
  }
  if (fi_broadcast(ep, &bval, 1, NULL, coll_addr, world[root],
                   FI_INT32, rank == root ? FI_SEND : FI_RECV, ctx)) {
    printf("FAIL: fi_broadcast\n"); return 1;
  }
  DRAIN("broadcast", rank == root ? FI_SEND : FI_RECV);
  if (bval != 0x1234) {
    printf("FAIL: broadcast rank %d got %d expected %d\n", rank, bval, 0x1234);
    return 1;
  }

  // --- reduce (SUM to root): root must see n(n+1)/2 ---
  ctx = (void*) 0x4ED;
  int32_t rsrc = rank + 1, rdst = -1;
  if (fi_reduce(ep, &rsrc, 1, NULL, &rdst, NULL, coll_addr,
                world[root], FI_INT32, FI_SUM, 0, ctx)) {
    printf("FAIL: fi_reduce\n"); return 1;
  }
  DRAIN("reduce", rank == root ? FI_SEND | FI_RECV : FI_SEND);
  if (rank == root && rdst != sum_expected) {
    printf("FAIL: reduce root got %d expected %d\n", rdst, sum_expected);
    return 1;
  }

  // --- allreduce (SUM): everyone must see n(n+1)/2; algorithm from registry ---
  ctx = (void*) 0xC0FFEE;
  int32_t src = rank + 1, result = -1;
  if (fi_allreduce(ep, &src, 1, NULL, &result, NULL, coll_addr,
                   FI_INT32, FI_SUM, 0, ctx)) {
    printf("FAIL: fi_allreduce\n"); return 1;
  }
  DRAIN("allreduce", FI_SEND | FI_RECV);
  if (result != sum_expected) {
    printf("FAIL: allreduce rank %d got %d expected %d\n",
           rank, result, sum_expected);
    return 1;
  }

  size_t too_many = (size_t) INT_MAX / sizeof(int32_t) + 1;
  ssize_t huge_ret = fi_allreduce(ep, &src, too_many, NULL,
                                  &result, NULL, coll_addr,
                                  FI_INT32, FI_SUM, 0, NULL);
  if (huge_ret != -FI_EMSGSIZE) {
    printf("FAIL: oversized allreduce returned %zd expected %d\n",
           huge_ret, -FI_EMSGSIZE);
    return 1;
  }

  size_t too_many_allgather =
      (size_t) INT_MAX / sizeof(int32_t) / (size_t) size + 1;
  huge_ret = fi_allgather(ep, &src, too_many_allgather, NULL,
                          &result, NULL, coll_addr, FI_INT32, 0, NULL);
  if (huge_ret != -FI_EMSGSIZE) {
    printf("FAIL: oversized allgather returned %zd expected %d\n",
           huge_ret, -FI_EMSGSIZE);
    return 1;
  }

  // The vector collectives use size-element buffers; keep the test bounded.
  if (size >= 2 && size <= 64) {
    // --- allgather: result[i] must equal contribution of rank i (= i+1) ---
    ctx = (void*) 0xA11;
    int32_t agsrc = rank + 1, agdst[64];
    for (int i = 0; i < size; ++i) agdst[i] = -1;
    if (fi_allgather(ep, &agsrc, 1, NULL, agdst, NULL, coll_addr,
                     FI_INT32, 0, ctx)) { printf("FAIL: fi_allgather\n"); return 1; }
    DRAIN("allgather", FI_SEND | FI_RECV);
    for (int i = 0; i < size; ++i) if (agdst[i] != i + 1) {
      printf("FAIL: allgather[%d]=%d expected %d\n", i, agdst[i], i + 1); return 1;
    }

    // --- gather to root: root's result[i] must equal i+1 ---
    ctx = (void*) 0x6A0;
    int32_t gsrc = rank + 1, gdst[64];
    for (int i = 0; i < size; ++i) gdst[i] = -1;
    if (fi_gather(ep, &gsrc, 1, NULL, gdst, NULL, coll_addr, world[root],
                  FI_INT32, 0, ctx)) { printf("FAIL: fi_gather\n"); return 1; }
    DRAIN("gather", rank == root ? FI_SEND | FI_RECV : FI_SEND);
    if (rank == root) for (int i = 0; i < size; ++i) if (gdst[i] != i + 1) {
      printf("FAIL: gather[%d]=%d expected %d\n", i, gdst[i], i + 1); return 1;
    }

    // --- scatter from root: rank r must receive r+1 ---
    ctx = (void*) 0x5CA;
    int32_t ssrc[64], sdst = -1;
    if (rank == root) for (int i = 0; i < size; ++i) ssrc[i] = i + 1;
    if (fi_scatter(ep, ssrc, 1, NULL, &sdst, NULL, coll_addr, world[root],
                   FI_INT32, 0, ctx)) { printf("FAIL: fi_scatter\n"); return 1; }
    DRAIN("scatter", rank == root ? FI_SEND | FI_RECV : FI_RECV);
    if (sdst != rank + 1) {
      printf("FAIL: scatter rank %d got %d expected %d\n", rank, sdst, rank + 1);
      return 1;
    }

    // Repeat with a nonzero root, including the two-rank midpoint-root case.
    const int scatter_root = size - 1;
    ctx = (void*) 0x5CA2;
    for (int i = 0; i < size; ++i) ssrc[i] = i + 10;
    sdst = -1;
    if (fi_scatter(ep, ssrc, 1, NULL, &sdst, NULL, coll_addr,
                   world[scatter_root], FI_INT32, 0, ctx)) {
      printf("FAIL: nonzero-root fi_scatter\n"); return 1;
    }
    DRAIN("nonzero-root scatter",
          rank == scatter_root ? FI_SEND | FI_RECV : FI_RECV);
    if (sdst != rank + 10) {
      printf("FAIL: nonzero-root scatter rank %d got %d expected %d\n",
             rank, sdst, rank + 10);
      return 1;
    }

  }

  // The engine actor is still a stub; the provider must reject this operation
  // instead of entering it and aborting the simulation.
  int32_t rs_src = rank + 1, rs_dst = -1;
  ssize_t rs_ret = fi_reduce_scatter(ep, &rs_src, 1, NULL, &rs_dst, NULL,
                                     coll_addr, FI_INT32, FI_SUM, 0, NULL);
  if (rs_ret != -FI_EOPNOTSUPP) {
    printf("FAIL: fi_reduce_scatter returned %zd expected %d\n",
           rs_ret, -FI_EOPNOTSUPP);
    return 1;
  }

  // A second join over the same membership creates a distinct group.
  struct fid_ep* ep2 = NULL;
  CHECK(fi_endpoint(dom, info, &ep2, NULL), "fi_endpoint(ep2)");
  struct fid_cq* cq2 = NULL;
  CHECK(fi_cq_open(dom, &cq_attr, &cq2, NULL), "fi_cq_open(cq2)");
  CHECK(fi_ep_bind(ep2, &cq2->fid, FI_TRANSMIT | FI_RECV), "fi_ep_bind(ep2,cq2)");
  CHECK(fi_ep_bind(ep2, &av->fid, 0), "fi_ep_bind(ep2,av)");
  struct fid_eq* eq2 = NULL;
  CHECK(fi_eq_open(fab, &eq_attr, &eq2, NULL), "fi_eq_open(eq2)");
  CHECK(fi_ep_bind(ep2, &eq2->fid, 0), "fi_ep_bind(ep2,eq2)");
  CHECK(fi_enable(ep2), "fi_enable(ep2)");
  struct fid_mc* mc2 = NULL;
  void* join2_ctx = (void*) 0xC022EC7;
  CHECK(fi_join_collective(ep2, FI_ADDR_NOTAVAIL, world_set, 0, &mc2, join2_ctx),
        "fi_join_collective(ep2)");
  memset(&join_entry, 0, sizeof(join_entry));
  event = 0;
  eq_read = fi_eq_sread(eq2, &event, &join_entry, sizeof(join_entry), -1, 0);
  if (eq_read != (ssize_t) sizeof(join_entry) || event != FI_JOIN_COMPLETE ||
      join_entry.fid != &mc2->fid || join_entry.context != join2_ctx) {
    printf("FAIL: ep2 join completion\n");
    return 1;
  }

  struct fid_mc* child_mc = NULL;
  ssize_t child_join = fi_join_collective(ep, mc->fi_addr, world_set, 0,
                                          &child_mc, NULL);
  if (child_join != -FI_EOPNOTSUPP) {
    printf("FAIL: provider-managed subgroup join returned %zd expected %d\n",
           child_join, -FI_EOPNOTSUPP);
    return 1;
  }

  void* group2_ctx = (void*) 0x220;
  int32_t group2_src = rank + 1, group2_result = -1;
  if (fi_allreduce(ep2, &group2_src, 1, NULL, &group2_result, NULL,
                   mc2->fi_addr, FI_INT32, FI_SUM, 0, group2_ctx)) {
    printf("FAIL: second-group fi_allreduce\n"); return 1;
  }
  memset(&comp, 0, sizeof(comp));
  ssize_t group2_read = fi_cq_sread(cq2, &comp, 1, NULL, -1);
  if (group2_read != 1 || comp.op_context != group2_ctx ||
      comp.flags != (FI_COLLECTIVE | FI_SEND | FI_RECV) ||
      group2_result != sum_expected) {
    printf("FAIL: second-group completion/result rank %d read=%zd result=%d\n",
           rank, group2_read, group2_result);
    return 1;
  }

  // A scalable-endpoint context must expose the collective ops table too.
  struct fid_ep* sep = NULL;
  struct fid_ep* sep_tx = NULL;
  CHECK(fi_scalable_ep(dom, info, &sep, NULL), "fi_scalable_ep");
  CHECK(fi_tx_context(sep, 0, NULL, &sep_tx, NULL), "fi_tx_context");
  if (!sep_tx || !sep_tx->collective) {
    printf("FAIL: SEP tx context has no collective ops\n");
    return 1;
  }
  struct fid_cq* sep_cq = NULL;
  struct fid_eq* sep_eq = NULL;
  CHECK(fi_cq_open(dom, &cq_attr, &sep_cq, NULL), "fi_cq_open(sep)");
  CHECK(fi_eq_open(fab, &eq_attr, &sep_eq, NULL), "fi_eq_open(sep)");
  CHECK(fi_ep_bind(sep_tx, &sep_cq->fid, FI_TRANSMIT | FI_RECV),
        "fi_ep_bind(sep,cq)");
  CHECK(fi_ep_bind(sep_tx, &av->fid, 0), "fi_ep_bind(sep,av)");
  CHECK(fi_ep_bind(sep_tx, &sep_eq->fid, 0), "fi_ep_bind(sep,eq)");
  CHECK(fi_enable(sep_tx), "fi_enable(sep)");
  struct fid_mc* sep_mc = NULL;
  void* sep_join_ctx = (void*) 0x5E90;
  CHECK(fi_join_collective(sep_tx, FI_ADDR_NOTAVAIL, world_set, 0,
                           &sep_mc, sep_join_ctx),
        "fi_join_collective(sep)");
  eq_read = fi_eq_sread(sep_eq, &event, &join_entry,
                        sizeof(join_entry), -1, 0);
  if (eq_read != (ssize_t) sizeof(join_entry) || event != FI_JOIN_COMPLETE ||
      join_entry.fid != &sep_mc->fid) {
    printf("FAIL: SEP join completion\n"); return 1;
  }
  int32_t sep_src = rank + 1, sep_result = -1;
  void* sep_ctx = (void*) 0x5E91;
  if (fi_allreduce(sep_tx, &sep_src, 1, NULL, &sep_result, NULL,
                   sep_mc->fi_addr, FI_INT32, FI_SUM, 0, sep_ctx)) {
    printf("FAIL: SEP fi_allreduce\n"); return 1;
  }
  memset(&comp, 0, sizeof(comp));
  ssize_t sep_read = fi_cq_sread(sep_cq, &comp, 1, NULL, -1);
  if (sep_read != 1 || comp.op_context != sep_ctx ||
      comp.flags != (FI_COLLECTIVE | FI_SEND | FI_RECV) ||
      sep_result != sum_expected) {
    printf("FAIL: SEP completion/result rank %d read=%zd result=%d\n",
           rank, sep_read, sep_result);
    return 1;
  }
  CHECK(fi_close(&sep_mc->fid), "fi_close(sep_mc)");
  CHECK(fi_close(&sep_tx->fid), "fi_close(sep_tx)");
  CHECK(fi_close(&sep->fid), "fi_close(sep)");
  CHECK(fi_close(&sep_cq->fid), "fi_close(sep_cq)");
  CHECK(fi_close(&sep_eq->fid), "fi_close(sep_eq)");

  struct fid_ep* ep_no_cq = NULL;
  CHECK(fi_endpoint(dom, info, &ep_no_cq, NULL), "fi_endpoint(no-cq)");
  CHECK(fi_ep_bind(ep_no_cq, &av->fid, 0), "fi_ep_bind(no-cq,av)");
  struct fid_eq* eq_no_cq = NULL;
  CHECK(fi_eq_open(fab, &eq_attr, &eq_no_cq, NULL), "fi_eq_open(no-cq)");
  CHECK(fi_ep_bind(ep_no_cq, &eq_no_cq->fid, 0), "fi_ep_bind(no-cq,eq)");
  CHECK(fi_enable(ep_no_cq), "fi_enable(no-cq)");
  struct fid_mc* mc_no_cq = NULL;
  void* no_cq_join_ctx = (void*) 0xC0C0;
  CHECK(fi_join_collective(ep_no_cq, FI_ADDR_NOTAVAIL, world_set, 0,
                           &mc_no_cq, no_cq_join_ctx),
        "fi_join_collective(no-cq)");
  eq_read = fi_eq_sread(eq_no_cq, &event, &join_entry,
                        sizeof(join_entry), -1, 0);
  if (eq_read != (ssize_t) sizeof(join_entry) || event != FI_JOIN_COMPLETE) {
    printf("FAIL: no-cq join completion\n"); return 1;
  }
  ssize_t no_cq_ret = fi_barrier(ep_no_cq, mc_no_cq->fi_addr, NULL);
  if (no_cq_ret != -FI_ENOCQ) {
    printf("FAIL: no-cq barrier returned %zd expected %d\n",
           no_cq_ret, -FI_ENOCQ);
    return 1;
  }

  // This is one new group, but its local endpoint differs across ranks.
  struct fid_ep* mixed_ep = (rank % 2) ? ep2 : ep;
  struct fid_eq* mixed_eq = (rank % 2) ? eq2 : eq;
  struct fid_cq* mixed_cq = (rank % 2) ? cq2 : cq;
  struct fid_mc* mixed_mc = NULL;
  void* mixed_join_ctx = (void*) 0x2E1;
  CHECK(fi_join_collective(mixed_ep, FI_ADDR_NOTAVAIL, world_set, 0,
                           &mixed_mc, mixed_join_ctx),
        "fi_join_collective(mixed endpoint)");
  memset(&join_entry, 0, sizeof(join_entry));
  event = 0;
  eq_read = fi_eq_sread(mixed_eq, &event, &join_entry,
                        sizeof(join_entry), -1, 0);
  if (eq_read != (ssize_t) sizeof(join_entry) || event != FI_JOIN_COMPLETE ||
      join_entry.fid != &mixed_mc->fid ||
      join_entry.context != mixed_join_ctx) {
    printf("FAIL: mixed-endpoint join completion\n"); return 1;
  }
  void* mixed_ctx = (void*) 0x2E0;
  int32_t mixed_src = rank + 1, mixed_result = -1;
  if (fi_allreduce(mixed_ep, &mixed_src, 1, NULL, &mixed_result, NULL,
                   mixed_mc->fi_addr, FI_INT32, FI_SUM, 0, mixed_ctx)) {
    printf("FAIL: mixed-endpoint fi_allreduce\n"); return 1;
  }
  memset(&comp, 0, sizeof(comp));
  ssize_t mixed_read = fi_cq_sread(mixed_cq, &comp, 1, NULL, -1);
  if (mixed_read != 1 || comp.op_context != mixed_ctx ||
      comp.flags != (FI_COLLECTIVE | FI_SEND | FI_RECV) ||
      mixed_result != sum_expected) {
    printf("FAIL: mixed-endpoint completion/result rank %d read=%zd result=%d\n",
           rank, mixed_read, mixed_result);
    return 1;
  }

  // Deliberately overlap first allreduces from two fresh groups. Both use
  // tag 0; group identity must keep the engine from merging their actors.
  struct fid_mc* collision_world_mc = NULL;
  void* collision_world_join_ctx = (void*) 0xC0111;
  CHECK(fi_join_collective(ep, FI_ADDR_NOTAVAIL, world_set, 0,
                           &collision_world_mc, collision_world_join_ctx),
        "fi_join_collective(collision world)");
  eq_read = fi_eq_sread(eq, &event, &join_entry, sizeof(join_entry), -1, 0);
  if (eq_read != (ssize_t) sizeof(join_entry) || event != FI_JOIN_COMPLETE)
    { printf("FAIL: collision-world join completion\n"); return 1; }

  struct fi_av_set_attr collision_even_attr;
  memset(&collision_even_attr, 0, sizeof(collision_even_attr));
  collision_even_attr.count = (size_t) ((size + 1) / 2);
  collision_even_attr.start_addr = world[0];
  collision_even_attr.end_addr = world[size - 1];
  collision_even_attr.stride = 2;
  struct fid_av_set* collision_even_set = NULL;
  CHECK(fi_av_set(av, &collision_even_attr, &collision_even_set, NULL),
        "fi_av_set(collision even)");
  struct fid_mc* collision_even_mc = NULL;
  if ((rank % 2) == 0) {
    CHECK(fi_join_collective(ep, FI_ADDR_NOTAVAIL, collision_even_set, 0,
                             &collision_even_mc, NULL),
          "fi_join_collective(collision even)");
    eq_read = fi_eq_sread(eq, &event, &join_entry,
                          sizeof(join_entry), -1, 0);
    if (eq_read != (ssize_t) sizeof(join_entry) || event != FI_JOIN_COMPLETE)
      { printf("FAIL: collision-even join completion\n"); return 1; }

    int32_t even_overlap_src = rank + 1, even_overlap_result = -1;
    ctx = (void*) 0xC0112;
    if (fi_allreduce(ep, &even_overlap_src, 1, NULL,
                     &even_overlap_result, NULL,
                     collision_even_mc->fi_addr, FI_INT32, FI_SUM, 0, ctx)) {
      printf("FAIL: collision-even allreduce\n"); return 1;
    }
    DRAIN("collision-even allreduce", FI_SEND | FI_RECV);
    int collision_even_size = (size + 1) / 2;
    if (even_overlap_result != collision_even_size * collision_even_size) {
      printf("FAIL: collision-even result rank %d got %d expected %d\n",
             rank, even_overlap_result,
             collision_even_size * collision_even_size);
      return 1;
    }
  }

  int32_t world_overlap_src = rank + 1, world_overlap_result = -1;
  ctx = (void*) 0xC0113;
  if (fi_allreduce(ep, &world_overlap_src, 1, NULL, &world_overlap_result,
                   NULL, collision_world_mc->fi_addr,
                   FI_INT32, FI_SUM, 0, ctx)) {
    printf("FAIL: collision-world allreduce\n"); return 1;
  }
  DRAIN("collision-world allreduce", FI_SEND | FI_RECV);
  if (world_overlap_result != sum_expected) {
    printf("FAIL: collision-world result rank %d got %d expected %d\n",
           rank, world_overlap_result, sum_expected);
    return 1;
  }
  if (collision_even_mc)
    CHECK(fi_close(&collision_even_mc->fid), "fi_close(collision_even_mc)");
  CHECK(fi_close(&collision_world_mc->fid), "fi_close(collision_world_mc)");
  CHECK(fi_close(&collision_even_set->fid), "fi_close(collision_even_set)");

  // Join one group with role-specific access: root sends, peers receive.
  struct fid_mc* access_mc = NULL;
  void* access_join_ctx = (void*) 0xACC355;
  uint64_t access_flags = rank == root ? FI_SEND : FI_RECV;
  CHECK(fi_join_collective(ep, FI_ADDR_NOTAVAIL, world_set, access_flags,
                           &access_mc, access_join_ctx),
        "fi_join_collective(access restricted)");
  memset(&join_entry, 0, sizeof(join_entry));
  event = 0;
  eq_read = fi_eq_sread(eq, &event, &join_entry, sizeof(join_entry), -1, 0);
  if (eq_read != (ssize_t) sizeof(join_entry) || event != FI_JOIN_COMPLETE ||
      join_entry.fid != &access_mc->fid) {
    printf("FAIL: access-restricted join completion\n"); return 1;
  }
  int32_t denied_result = -1;
  ssize_t denied = fi_allreduce(ep, &mixed_src, 1, NULL, &denied_result, NULL,
                                access_mc->fi_addr, FI_INT32, FI_SUM, 0, NULL);
  if (denied != -FI_EACCES) {
    printf("FAIL: access-restricted allreduce returned %zd expected %d\n",
           denied, -FI_EACCES);
    return 1;
  }
  ctx = (void*) 0xACC;
  int32_t access_bval = rank == root ? 0x1357 : -1;
  if (fi_broadcast(ep, &access_bval, 1, NULL, access_mc->fi_addr,
                   world[root], FI_INT32,
                   rank == root ? FI_SEND : FI_RECV, ctx)) {
    printf("FAIL: access-restricted broadcast\n"); return 1;
  }
  DRAIN("access-restricted broadcast", access_flags);
  if (access_bval != 0x1357) {
    printf("FAIL: access-restricted broadcast rank %d got %d\n",
           rank, access_bval);
    return 1;
  }

  // A completed join owns its membership independently of the AV set.
  CHECK(fi_close(&world_set->fid), "fi_close(world_set)");
  ctx = (void*) 0xC105E;
  int32_t closed_set_bval = rank == root ? 0x2468 : -1;
  if (fi_broadcast(ep, &closed_set_bval, 1, NULL, coll_addr, world[root],
                   FI_INT32, rank == root ? FI_SEND : FI_RECV, ctx)) {
    printf("FAIL: post-AV-set-close broadcast\n"); return 1;
  }
  DRAIN("post-AV-set-close broadcast", rank == root ? FI_SEND : FI_RECV);
  if (closed_set_bval != 0x2468) {
    printf("FAIL: post-AV-set-close broadcast rank %d got %d\n",
           rank, closed_set_bval);
    return 1;
  }

  // Exercise a true subgroup and an encoded, nonzero root address. Even global
  // ranks form the group; the final even rank is root.
  struct fi_av_set_attr even_attr;
  memset(&even_attr, 0, sizeof(even_attr));
  const int even_size = (size + 1) / 2;
  even_attr.count = (size_t) even_size;
  even_attr.start_addr = world[0];
  even_attr.end_addr = world[size - 1];
  even_attr.stride = 2;
  struct fid_av_set* even_set = NULL;
  CHECK(fi_av_set(av, &even_attr, &even_set, NULL), "fi_av_set(even)");
  if ((rank % 2) == 0) {
    struct fid_mc* even_mc = NULL;
    void* even_join_ctx = (void*) 0xE7E0;
    CHECK(fi_join_collective(ep, FI_ADDR_NOTAVAIL, even_set, 0,
                             &even_mc, even_join_ctx),
          "fi_join_collective(even)");
    memset(&join_entry, 0, sizeof(join_entry));
    event = 0;
    eq_read = fi_eq_sread(eq, &event, &join_entry, sizeof(join_entry), -1, 0);
    if (eq_read != (ssize_t) sizeof(join_entry) || event != FI_JOIN_COMPLETE ||
        join_entry.fid != &even_mc->fid || join_entry.context != even_join_ctx) {
      printf("FAIL: even join completion\n");
      return 1;
    }

    const int even_root = 2 * (even_size - 1);
    ctx = (void*) 0xE7B;
    int32_t even_bval = (rank == even_root) ? 0x5678 : -1;
    if (fi_broadcast(ep, &even_bval, 1, NULL, even_mc->fi_addr,
                     world[even_root], FI_INT32,
                     rank == even_root ? FI_SEND : FI_RECV, ctx)) {
      printf("FAIL: even fi_broadcast\n"); return 1;
    }
    DRAIN("even broadcast", rank == even_root ? FI_SEND : FI_RECV);
    if (even_bval != 0x5678) {
      printf("FAIL: even broadcast rank %d got %d\n", rank, even_bval);
      return 1;
    }

    ctx = (void*) 0xE7A;
    int32_t even_src = rank + 1, even_result = -1;
    if (fi_allreduce(ep, &even_src, 1, NULL, &even_result, NULL,
                     even_mc->fi_addr, FI_INT32, FI_SUM, 0, ctx)) {
      printf("FAIL: even fi_allreduce\n"); return 1;
    }
    DRAIN("even allreduce", FI_SEND | FI_RECV);
    if (even_result != even_size * even_size) {
      printf("FAIL: even allreduce rank %d got %d expected %d\n",
             rank, even_result, even_size * even_size);
      return 1;
    }
  }

  if (rank == 0) {
    const char* alg = getenv("SUMI_ALLREDUCE_ALG");
    if (!alg) alg = getenv("SUMI_ALLREDUCE_PARAM");
    const char* vec = (size >= 2) ? ",allgather,gather,scatter" : "";
    printf("PASS: fi_collectives (%d ranks; barrier,broadcast,reduce,allreduce%s,subgroup;"
           " SUM=%d, allreduce_alg=%s)\n",
           size, vec, result, alg ? alg : "default");
  }
  if (skew_cq) CHECK(fi_close(&skew_cq->fid), "fi_close(skew)");
  return 0;
}
