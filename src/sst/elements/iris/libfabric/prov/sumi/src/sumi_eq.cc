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
 * Copyright (c) 2015-2016 Cray Inc.  All rights reserved.
 * Copyright (c) 2015 Los Alamos National Security, LLC. All rights reserved.
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

#include <assert.h>

#include <stdlib.h>

#include "sumi_prov.h"
#include "sumi_wait.h"

#include <string.h>
#include <deque>
#include <vector>


#define GNIX_EQ_DEFAULT_SIZE 1000

// A minimal but functional event queue. The provider only needs to deliver
// FI_JOIN_COMPLETE (from fi_join_collective) today, but read/sread/write are
// implemented generally so any provider-posted event reaches the client. Each
// stored event carries a fi_eq_entry payload (fid/context/data); errors and
// larger event structs are not modeled (spike).
namespace {
struct SumiEqEvent {
  uint32_t event;
  struct fi_eq_entry entry;
};
struct SumiEqEventList {
  std::deque<SumiEqEvent> q;
};
} // namespace

DIRECT_FN STATIC ssize_t sumi_eq_read(struct fid_eq *eq, uint32_t *event,
              void *buf, size_t len, uint64_t flags);

DIRECT_FN STATIC ssize_t sumi_eq_readerr(struct fid_eq *eq,
           struct fi_eq_err_entry *buf,
           uint64_t flags);

DIRECT_FN STATIC ssize_t sumi_eq_write(struct fid_eq *eq, uint32_t event,
               const void *buf, size_t len,
               uint64_t flags);

DIRECT_FN STATIC ssize_t sumi_eq_sread(struct fid_eq *eq, uint32_t *event,
               void *buf, size_t len, int timeout,
               uint64_t flags);

DIRECT_FN STATIC const char *sumi_eq_strerror(struct fid_eq *eq, int prov_errno,
                const void *err_data, char *buf,
                size_t len);

EXTERN_C DIRECT_FN STATIC  int sumi_eq_close(struct fid *fid);

EXTERN_C DIRECT_FN STATIC  int sumi_eq_control(struct fid *eq, int command, void *arg);

static struct fi_ops_eq sumi_eq_ops = {
  .size = sizeof(struct fi_ops_eq),
  .read = sumi_eq_read,
  .readerr = sumi_eq_readerr,
  .write = sumi_eq_write,
  .sread = sumi_eq_sread,
  .strerror = sumi_eq_strerror
};

static struct fi_ops sumi_fi_eq_ops = {
  .size = sizeof(struct fi_ops),
  .close = sumi_eq_close,
  .bind = fi_no_bind,
  .control = sumi_eq_control,
  .ops_open = fi_no_ops_open
};

extern "C" DIRECT_FN  int sumi_eq_open(struct fid_fabric *fabric, struct fi_eq_attr *attr, struct fid_eq **eq_ptr, void *context)
{
	if (!fabric)
		return -FI_EINVAL;

  struct sumi_fid_eq *eq = (sumi_fid_eq*) calloc(1, sizeof(sumi_fid_eq));
  if (!eq)
		return -FI_ENOMEM;

  ErrorDeallocate err(eq, [](void* ptr){
    auto* eq = (sumi_fid_eq*) ptr;
    free(eq);
  });

  if (!attr)
    return -FI_EINVAL;

  if (!attr->size)
    attr->size = GNIX_EQ_DEFAULT_SIZE;

  // Only support FI_WAIT_SET and FI_WAIT_UNSPEC
  switch (attr->wait_obj) {
  case FI_WAIT_NONE:
    break;
  case FI_WAIT_SET: {
    if (!attr->wait_set) {
      //must be given a wait set!
      return -FI_EINVAL;
    }
    eq->wait = attr->wait_set;
    sstmaci_add_wait(eq->wait, &eq->eq_fid.fid);
    break;
  }
  case FI_WAIT_UNSPEC: {
    struct fi_wait_attr requested = {
      .wait_obj = eq->attr.wait_obj,
      .flags = 0
    };
    sumi_wait_open(&eq->fabric->fab_fid, &requested, &eq->wait);
    sstmaci_add_wait(eq->wait, &eq->eq_fid.fid);
    break;
  }
  default:
    return -FI_ENOSYS;
  }

  eq->fabric = (sumi_fid_fabric*) fabric;   // needed by fi_ep_bind's fabric check
  eq->eq_fid.fid.fclass = FI_CLASS_EQ;
  eq->eq_fid.fid.context = context;
  eq->eq_fid.fid.ops = &sumi_fi_eq_ops;
  eq->eq_fid.ops = &sumi_eq_ops;
  eq->attr = *attr;
  eq->events = new SumiEqEventList();

  *eq_ptr = (fid_eq*) &eq->eq_fid;

  err.success();
  return FI_SUCCESS;
}

// Enqueue an event; the client observes it via fi_eq_read/sread.
void sumi_eq_post(struct sumi_fid_eq* eq, uint32_t event, fid_t fid,
                  void* context, uint64_t data)
{
  if (!eq || !eq->events) return;
  SumiEqEvent e;
  e.event = event;
  e.entry.fid = fid;
  e.entry.context = context;
  e.entry.data = data;
  ((SumiEqEventList*) eq->events)->q.push_back(e);
}

EXTERN_C DIRECT_FN STATIC  int sumi_eq_close(struct fid *fid)
{
  if (fid) {
    sumi_fid_eq* eq = container_of(fid, struct sumi_fid_eq, eq_fid);
    delete (SumiEqEventList*) eq->events;
    eq->events = nullptr;
    free(eq);
  }
  return FI_SUCCESS;
#if 0
	struct sumi_fid_eq *eq;
	int references_held;

	SUMI_TRACE(FI_LOG_EQ, "\n");

	if (!fid)
		return -FI_EINVAL;

	eq = container_of(fid, struct sumi_fid_eq, eq_fid);

	references_held = _sumi_ref_put(eq);
	if (references_held) {
		SUMI_INFO(FI_LOG_EQ, "failed to fully close eq due "
				"to lingering references. references=%i eq=%p\n",
				references_held, eq);
	}
#endif
	return FI_SUCCESS;
}


DIRECT_FN STATIC ssize_t sumi_eq_read(struct fid_eq *eq, uint32_t *event,
              void *buf, size_t len, uint64_t flags)
{
  sumi_fid_eq* eq_priv = container_of(eq, struct sumi_fid_eq, eq_fid);
  SumiEqEventList* q = (SumiEqEventList*) eq_priv->events;
  if (!q || q->q.empty()) return -FI_EAGAIN;

  const SumiEqEvent& e = q->q.front();
  if (event) *event = e.event;
  size_t n = len < sizeof(struct fi_eq_entry) ? len : sizeof(struct fi_eq_entry);
  if (buf) memcpy(buf, &e.entry, n);
  if (!(flags & FI_PEEK)) q->q.pop_front();
  return (ssize_t) n;
}

// The join completion is posted synchronously (sumi_cm_join), so by the time a
// client sreads it the event is already queued: a plain read satisfies the
// blocking-wait contract. A fully deferred join would instead pump the
// simulator here until the event lands.
DIRECT_FN STATIC ssize_t sumi_eq_sread(struct fid_eq *eq, uint32_t *event,
               void *buf, size_t len, int timeout,
               uint64_t flags)
{
  return sumi_eq_read(eq, event, buf, len, flags);
}

EXTERN_C DIRECT_FN STATIC  int sumi_eq_control(struct fid *eq, int command, void *arg)
{
  switch (command) {
  case FI_GETWAIT:
    /* return _sumi_get_wait_obj(eq_priv->wait, arg); */
    return -FI_ENOSYS;
  default:
    return -FI_EINVAL;
  }
}

DIRECT_FN STATIC ssize_t sumi_eq_readerr(struct fid_eq *eq,
					 struct fi_eq_err_entry *buf,
					 uint64_t flags)
{
  ssize_t read_size = sizeof(*buf);
#if 0
	struct sumi_fid_eq *eq_priv;
	struct sumi_eq_entry *entry;
	struct slist_entry *item;
	struct sumi_eq_err_buf *err_buf;
	struct fi_eq_err_entry *fi_err;



	eq_priv = container_of(eq, struct sumi_fid_eq, eq_fid);

	fastlock_acquire(&eq_priv->lock);

	if (flags & FI_PEEK)
		item = _sumi_queue_peek(eq_priv->errors);
	else
		item = _sumi_queue_dequeue(eq_priv->errors);

	if (!item) {
		read_size = -FI_EAGAIN;
		goto err;
	}

	entry = container_of(item, struct sumi_eq_entry, item);
	fi_err = (struct fi_eq_err_entry *)entry->the_entry;

	memcpy(buf, entry->the_entry, read_size);

	/* If removing an event with err_data, mark err buf to be freed during
	 * the next EQ read. */
	if (!(flags & FI_PEEK) && fi_err->err_data) {
		err_buf = container_of(fi_err->err_data,
				       struct sumi_eq_err_buf, buf);
		err_buf->do_free = 1;
	}

	_sumi_queue_enqueue_free(eq_priv->errors, &entry->item);

err:
	fastlock_release(&eq_priv->lock);
#endif
	return read_size;
}

DIRECT_FN STATIC ssize_t sumi_eq_write(struct fid_eq *eq, uint32_t event,
				       const void *buf, size_t len,
				       uint64_t flags)
{
  sumi_fid_eq* eq_priv = container_of(eq, struct sumi_fid_eq, eq_fid);
  SumiEqEventList* q = (SumiEqEventList*) eq_priv->events;
  if (!q) return -FI_EINVAL;
  SumiEqEvent e;
  e.event = event;
  memset(&e.entry, 0, sizeof(e.entry));
  size_t n = len < sizeof(struct fi_eq_entry) ? len : sizeof(struct fi_eq_entry);
  if (buf) memcpy(&e.entry, buf, n);
  q->q.push_back(e);
  return (ssize_t) len;
}

DIRECT_FN STATIC const char *sumi_eq_strerror(struct fid_eq *eq, int prov_errno,
					      const void *err_data, char *buf,
					      size_t len)
{
	return NULL;
}

