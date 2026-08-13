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

/*
 * Copyright (c) 2015-2017 Cray Inc. All rights reserved.
 * Copyright (c) 2015-2017 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2019      Triad National Security, LLC.
 *                         All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

//
// Address vector common code
//
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <mercury/common/errors.h>

#include <algorithm>
#include <mutex>
#include <new>
#include <vector>

#include "sumi_prov.h"
#include "sumi_av.h"

EXTERN_C DIRECT_FN STATIC  int sumi_av_insert(struct fid_av *av, const void *addr,
				    size_t count, fi_addr_t *fi_addr,
            uint64_t flags, void *context);

EXTERN_C DIRECT_FN STATIC  int sumi_av_insertsvc(struct fid_av *av, const char *node,
				       const char *service, fi_addr_t *fi_addr,
				       uint64_t flags, void *context);

EXTERN_C DIRECT_FN STATIC  int sumi_av_insertsym(struct fid_av *av, const char *node,
				       size_t nodecnt, const char *service,
				       size_t svccnt, fi_addr_t *fi_addr,
				       uint64_t flags, void *context);

EXTERN_C DIRECT_FN STATIC  int sumi_av_remove(struct fid_av *av, fi_addr_t *fi_addr,
				    size_t count, uint64_t flags);

EXTERN_C DIRECT_FN STATIC  int sumi_av_lookup(struct fid_av *av, fi_addr_t fi_addr,
				    void *addr, size_t *addrlen);

DIRECT_FN const char *sumi_av_straddr(struct fid_av *av,
		const void *addr, char *buf,
		size_t *len);

static int sumi_av_close(fid_t fid);

namespace {

struct SumiAvState {
  std::mutex lock;
  std::vector<fi_addr_t> addrs;
};

SumiAvState* avState(sumi_fid_av* av)
{
  return static_cast<SumiAvState*>(av->state);
}

int reserveSet(sumi_fid_av_set* set, size_t capacity)
{
  if (capacity <= set->capacity) return FI_SUCCESS;
  if (capacity > SIZE_MAX / sizeof(fi_addr_t)) return -FI_EOVERFLOW;
  auto* addrs = static_cast<fi_addr_t*>(
      realloc(set->addrs, capacity * sizeof(fi_addr_t)));
  if (!addrs) return -FI_ENOMEM;
  set->addrs = addrs;
  set->capacity = capacity;
  return FI_SUCCESS;
}

bool contains(const sumi_fid_av_set* set, fi_addr_t addr)
{
  for (size_t i = 0; i < set->count; ++i) {
    if (set->addrs[i] == addr) return true;
  }
  return false;
}

int sumi_av_set_close(fid_t fid)
{
  auto* set = reinterpret_cast<sumi_fid_av_set*>(fid);
  free(set->addrs);
  free(set);
  return FI_SUCCESS;
}

int sumi_av_set_union(struct fid_av_set* dst_, const struct fid_av_set* src_)
{
  auto* dst = reinterpret_cast<sumi_fid_av_set*>(dst_);
  auto* src = reinterpret_cast<const sumi_fid_av_set*>(src_);
  if (dst->av != src->av) return -FI_EINVAL;
  int ret = reserveSet(dst, dst->count + src->count);
  if (ret) return ret;
  for (size_t i = 0; i < src->count; ++i) {
    if (!contains(dst, src->addrs[i])) dst->addrs[dst->count++] = src->addrs[i];
  }
  return FI_SUCCESS;
}

int sumi_av_set_intersect(struct fid_av_set* dst_, const struct fid_av_set* src_)
{
  auto* dst = reinterpret_cast<sumi_fid_av_set*>(dst_);
  auto* src = reinterpret_cast<const sumi_fid_av_set*>(src_);
  if (dst->av != src->av) return -FI_EINVAL;
  size_t out = 0;
  for (size_t i = 0; i < dst->count; ++i) {
    if (contains(src, dst->addrs[i])) dst->addrs[out++] = dst->addrs[i];
  }
  dst->count = out;
  return FI_SUCCESS;
}

int sumi_av_set_diff(struct fid_av_set* dst_, const struct fid_av_set* src_)
{
  auto* dst = reinterpret_cast<sumi_fid_av_set*>(dst_);
  auto* src = reinterpret_cast<const sumi_fid_av_set*>(src_);
  if (dst->av != src->av) return -FI_EINVAL;
  size_t out = 0;
  for (size_t i = 0; i < dst->count; ++i) {
    if (!contains(src, dst->addrs[i])) dst->addrs[out++] = dst->addrs[i];
  }
  dst->count = out;
  return FI_SUCCESS;
}

int sumi_av_set_insert(struct fid_av_set* set_, fi_addr_t addr)
{
  auto* set = reinterpret_cast<sumi_fid_av_set*>(set_);
  if (contains(set, addr)) return -FI_EINVAL;
  int ret = reserveSet(set, set->count + 1);
  if (ret) return ret;
  set->addrs[set->count++] = addr;
  return FI_SUCCESS;
}

int sumi_av_set_remove(struct fid_av_set* set_, fi_addr_t addr)
{
  auto* set = reinterpret_cast<sumi_fid_av_set*>(set_);
  for (size_t i = 0; i < set->count; ++i) {
    if (set->addrs[i] != addr) continue;
    for (size_t j = i + 1; j < set->count; ++j)
      set->addrs[j - 1] = set->addrs[j];
    --set->count;
    return FI_SUCCESS;
  }
  return -FI_EINVAL;
}

int sumi_av_set_addr(struct fid_av_set* /*set*/, fi_addr_t* coll_addr)
{
  if (!coll_addr) return -FI_EINVAL;
  *coll_addr = FI_ADDR_NOTAVAIL;
  return FI_SUCCESS;
}

struct fi_ops sumi_av_set_fi_ops = {
  .size = sizeof(struct fi_ops),
  .close = sumi_av_set_close,
  .bind = fi_no_bind,
  .control = fi_no_control,
  .ops_open = fi_no_ops_open,
};

struct fi_ops_av_set sumi_av_set_ops = {
  .size = sizeof(struct fi_ops_av_set),
  .set_union = sumi_av_set_union,
  .intersect = sumi_av_set_intersect,
  .diff = sumi_av_set_diff,
  .insert = sumi_av_set_insert,
  .remove = sumi_av_set_remove,
  .addr = sumi_av_set_addr,
};

int sumi_av_create_set(struct fid_av* av_, struct fi_av_set_attr* attr,
                       struct fid_av_set** set_out, void* context)
{
  if (!av_ || !attr || !set_out || attr->flags) return -FI_EINVAL;
  if ((attr->comm_key && !attr->comm_key_size) ||
      (!attr->comm_key && attr->comm_key_size))
    return -FI_EINVAL;
  if (attr->comm_key) return -FI_EOPNOTSUPP;
  auto* av = reinterpret_cast<sumi_fid_av*>(av_);
  auto* state = avState(av);
  if (!state) return -FI_EINVAL;

  std::lock_guard<std::mutex> guard(state->lock);
  const size_t stride = attr->stride ? attr->stride : 1;
  std::vector<fi_addr_t> members;
  const bool empty = attr->start_addr == FI_ADDR_NOTAVAIL &&
                     attr->end_addr == FI_ADDR_NOTAVAIL;
  if ((attr->start_addr == FI_ADDR_NOTAVAIL) !=
      (attr->end_addr == FI_ADDR_NOTAVAIL))
    return -FI_EINVAL;
  if (!empty) {
    auto first = std::find(state->addrs.begin(), state->addrs.end(),
                           attr->start_addr);
    auto last = std::find(state->addrs.begin(), state->addrs.end(),
                          attr->end_addr);
    if (first == state->addrs.end() || last == state->addrs.end() ||
        first > last)
      return -FI_EINVAL;

    const size_t first_idx = static_cast<size_t>(first - state->addrs.begin());
    const size_t last_idx = static_cast<size_t>(last - state->addrs.begin());
    for (size_t idx = first_idx; idx <= last_idx;) {
      members.push_back(state->addrs[idx]);
      if (last_idx - idx < stride) break;
      idx += stride;
    }
  }
  if (attr->count && members.size() > attr->count) return -FI_EINVAL;

  auto* set = static_cast<sumi_fid_av_set*>(calloc(1, sizeof(sumi_fid_av_set)));
  if (!set) return -FI_ENOMEM;
  int ret = reserveSet(set, members.size());
  if (ret) {
    free(set);
    return ret;
  }
  std::copy(members.begin(), members.end(), set->addrs);
  set->count = members.size();
  set->av = av;
  set->av_set_fid.fid.fclass = FI_CLASS_AV_SET;
  set->av_set_fid.fid.context = context;
  set->av_set_fid.fid.ops = &sumi_av_set_fi_ops;
  set->av_set_fid.ops = &sumi_av_set_ops;
  *set_out = &set->av_set_fid;
  return FI_SUCCESS;
}

} // namespace

/*******************************************************************************
 * FI_OPS_* data structures.
 ******************************************************************************/
static struct fi_ops_av sumi_av_ops = {
  .size = sizeof(struct fi_ops_av),
  .insert = sumi_av_insert,
  .insertsvc = sumi_av_insertsvc,
  .insertsym = sumi_av_insertsym,
  .remove = sumi_av_remove,
  .lookup = sumi_av_lookup,
  .straddr = sumi_av_straddr,
  .av_set = sumi_av_create_set,
};

static struct fi_ops sumi_fi_av_ops = {
  .size = sizeof(struct fi_ops),
  .close = sumi_av_close,
  .bind = fi_no_bind,
  .control = fi_no_control,
  .ops_open = fi_no_ops_open
};

// FI_ADDR_STR format: "<rank10>.<cq5>" with zero-padding, exactly 16 chars
// plus a trailing NUL = 17 bytes. Round-trips (rank, cq) through the string
// form so av_insert(getname()) == binary-encoded fi_addr_t.
#define SUMI_MAX_ADDR_CHARS 16
#define SUMI_ADDR_FORMAT_STR "%010" PRIu32 ".%05" PRIu16
#define SUMI_MAX_ADDR_LEN (SUMI_MAX_ADDR_CHARS+1)
// FI_ADDR_SSTMAC string form: "<rank>.<cq>.<rx>" with max widths 10.5.5.
#define SUMI_SSTMAC_ADDR_STR_LEN (22 + 1)

/*
 * Note: this function (according to WG), is not intended to
 * typically be used in the critical path for messaging/rma/amo
 * requests
 */
EXTERN_C DIRECT_FN STATIC  int sumi_av_lookup(struct fid_av *av, fi_addr_t fi_addr,
				    void *addr, size_t *addrlen)
{
  sumi_fid_av* av_impl = (sumi_fid_av*) av;
  if (av_impl->domain->addr_format == FI_ADDR_SSTMAC){
    if (*addrlen < sizeof(uint64_t)){
      return -FI_EINVAL;
    }
    uint64_t* addr_int = (uint64_t*) addr;
    *addr_int = fi_addr;
  } else if (av_impl->domain->addr_format == FI_ADDR_STR){
    if (*addrlen < SUMI_MAX_ADDR_LEN){
      return -FI_EINVAL;
    }
    uint32_t rank = ADDR_RANK(fi_addr);
    uint16_t cq   = ADDR_CQ(fi_addr);
    snprintf((char*)addr, SUMI_MAX_ADDR_LEN, SUMI_ADDR_FORMAT_STR, rank, cq);
    *addrlen = SUMI_MAX_ADDR_LEN;
  } else {
    sst_hg_abort_printf("internal error: got addr format that isn't SSTMAC or STR");
  }
  return FI_SUCCESS;
}

EXTERN_C DIRECT_FN STATIC  int sumi_av_insert(struct fid_av *av, const void *addr,
				    size_t count, fi_addr_t *fi_addr,
				    uint64_t flags, void *context)
{
  sumi_fid_av* av_impl = (sumi_fid_av*) av;
  std::vector<fi_addr_t> inserted(count);
  if (av_impl->domain->addr_format == FI_ADDR_STR){
    static bool warned_legacy_addr = false;
    char* addr_str = (char*) addr;
    for (int i=0; i < count; ++i){
      // Each slot must be NUL-terminated inside SUMI_MAX_ADDR_LEN bytes.
      if (strnlen(addr_str, SUMI_MAX_ADDR_LEN) == (size_t)SUMI_MAX_ADDR_LEN){
        return -FI_EINVAL;
      }
      // Parse "<rank>.<cq>"; accept legacy "<rank>" by defaulting cq to 0.
      char* end = nullptr;
      uint32_t rank = (uint32_t) std::strtoul(addr_str, &end, 10);
      uint16_t cq = 0;
      if (end && *end == '.'){
        cq = (uint16_t) std::strtoul(end + 1, nullptr, 10);
      } else if (!warned_legacy_addr){
        fprintf(stderr,
                "WARNING: sumi av_insert accepted legacy \"<rank>\" "
                "FI_ADDR_STR form; expected \"<rank10>.<cq5>\" (cq=0)\n");
        warned_legacy_addr = true;
      }
      inserted[i] = ADDR_RANK_BITS((uint64_t)rank) | ADDR_CQ_BITS((uint64_t)cq);
      if (fi_addr) fi_addr[i] = inserted[i];
      addr_str += SUMI_MAX_ADDR_LEN;
    }
  } else if (av_impl->domain->addr_format == FI_ADDR_SSTMAC) {
    uint64_t* addr_list = (uint64_t*) addr;
    for (int i=0; i < count; ++i){
      inserted[i] = addr_list[i];
      if (fi_addr) fi_addr[i] = inserted[i];
    }
  } else {
    sst_hg_abort_printf("internal error: got addr format that isn't SSTMAC or STR");
  }
  auto* state = avState(av_impl);
  std::lock_guard<std::mutex> guard(state->lock);
  state->addrs.insert(state->addrs.end(), inserted.begin(), inserted.end());
  return static_cast<int>(count);
}

EXTERN_C DIRECT_FN STATIC  int sumi_av_insertsvc(struct fid_av *av, const char *node,
				       const char *service, fi_addr_t *fi_addr,
				       uint64_t flags, void *context)
{
	return -FI_ENOSYS;
}

EXTERN_C DIRECT_FN STATIC  int sumi_av_insertsym(struct fid_av *av, const char *node,
				       size_t nodecnt, const char *service,
				       size_t svccnt, fi_addr_t *fi_addr,
				       uint64_t flags, void *context)
{
	return -FI_ENOSYS;
}

EXTERN_C DIRECT_FN STATIC  int sumi_av_remove(struct fid_av *av, fi_addr_t *fi_addr,
				    size_t count, uint64_t flags)
{
  if (!av || (!fi_addr && count)) return -FI_EINVAL;
  auto* state = avState(reinterpret_cast<sumi_fid_av*>(av));
  std::lock_guard<std::mutex> guard(state->lock);
  for (size_t i = 0; i < count; ++i) {
    auto found = std::find(state->addrs.begin(), state->addrs.end(), fi_addr[i]);
    if (found != state->addrs.end()) state->addrs.erase(found);
  }
  return FI_SUCCESS;
}

DIRECT_FN const char *sumi_av_straddr(struct fid_av *av,
		const void *addr, char *buf,
		size_t *len)
{
  if (!addr || !len)
    return NULL;

  sumi_fid_av* av_impl = (sumi_fid_av*) av;
  size_t size;

  if (av_impl->domain->addr_format == FI_ADDR_STR){
    size = snprintf(buf, *len, "%s", (const char*)addr);
  } else if (av_impl->domain->addr_format == FI_ADDR_SSTMAC) {
    uint64_t* addr_ptr = reinterpret_cast<uint64_t*>(const_cast<void*>(addr));
    uint32_t rank = ADDR_RANK(*addr_ptr);
    uint16_t cq = ADDR_CQ(*addr_ptr);
    uint16_t rx = ADDR_QUEUE(*addr_ptr);
    size = snprintf(buf, *len, "%" PRIu32 ".%" PRIu16 ".%" PRIu16,
            rank, cq, rx);
  } else {
    sst_hg_abort_printf("internal error: got addr format that isn't SSTMAC or STR");
  }
  // Make sure that possibly truncated output is NUL-terminated.
  if (buf && *len)
    buf[*len - 1] = '\0';
  *len = size + 1;
  return buf;
}

static int sumi_av_close(fid_t fid)
{
  sumi_fid_av* av_impl = (sumi_fid_av*) fid;
  delete avState(av_impl);
  free(av_impl);
  return FI_SUCCESS;
}

extern "C" DIRECT_FN  int sumi_av_bind(struct fid_av *av, struct fid *fid, uint64_t flags)
{
	return -FI_ENOSYS;
}

extern "C" DIRECT_FN  int sumi_av_open(struct fid_domain *domain, struct fi_av_attr *attr,
			   struct fid_av **av, void *context)
{
  sumi_fid_av* av_impl = (sumi_fid_av*) calloc(1, sizeof(sumi_fid_av));
  if (!av_impl) return -FI_ENOMEM;
  av_impl->state = new (std::nothrow) SumiAvState;
  if (!av_impl->state) {
    free(av_impl);
    return -FI_ENOMEM;
  }
  av_impl->av_fid.fid.fclass = FI_CLASS_AV;
  av_impl->av_fid.fid.context = context;
  av_impl->av_fid.fid.ops = const_cast<fi_ops*>(&sumi_fi_av_ops);
  av_impl->av_fid.ops = const_cast<fi_ops_av*>(&sumi_av_ops);
  av_impl->domain = (sumi_fid_domain*) domain;
  *av = (fid_av*) av_impl;
  return FI_SUCCESS;
}
