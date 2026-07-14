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

// Registers the in-tree DAG collectives with the CollectiveRegistry so they
// can be selected by name (see collective_registry.h). Registration is
// name-only indirection: with no name selected, sim_transport.cc still runs
// each op's built-in default via the unchanged `new`, so behavior is identical.

#include <iris/sumi/collective_registry.h>
#include <iris/sumi/allreduce.h>
#include <iris/sumi/ring_allreduce.h>
#include <iris/sumi/reduce.h>
#include <iris/sumi/bcast.h>
#include <iris/sumi/reduce_scatter.h>
#include <iris/sumi/scan.h>
#include <iris/sumi/gather.h>
#include <iris/sumi/scatter.h>

namespace SST::Iris::sumi {

// allreduce: recursive-doubling (default) and bandwidth-optimal ring.
SUMI_REGISTER_COLLECTIVE(allreduce, "recdouble", WilkeHalvingAllreduce,
    a.engine, a.dst, a.src, a.nelems, a.type_size, a.tag, a.fxn, a.cq_id, a.comm)
SUMI_REGISTER_COLLECTIVE(allreduce, "ring", RingAllreduce,
    a.engine, a.dst, a.src, a.nelems, a.type_size, a.tag, a.fxn, a.cq_id, a.comm)

SUMI_REGISTER_COLLECTIVE(reduce, "recdouble", WilkeHalvingReduce,
    a.engine, a.root, a.dst, a.src, a.nelems, a.type_size, a.tag, a.fxn, a.cq_id, a.comm)

SUMI_REGISTER_COLLECTIVE(bcast, "btree", BinaryTreeBcastCollective,
    a.engine, a.root, a.dst, a.nelems, a.type_size, a.tag, a.cq_id, a.comm)

SUMI_REGISTER_COLLECTIVE(reduce_scatter, "halving", HalvingReduceScatter,
    a.engine, a.dst, a.src, a.nelems, a.type_size, a.tag, a.fxn, a.cq_id, a.comm)

SUMI_REGISTER_COLLECTIVE(scan, "btree", SimultaneousBtreeScan,
    a.engine, a.dst, a.src, a.nelems, a.type_size, a.tag, a.fxn, a.cq_id, a.comm)

SUMI_REGISTER_COLLECTIVE(gather, "btree", BtreeGather,
    a.engine, a.root, a.dst, a.src, a.nelems, a.type_size, a.tag, a.cq_id, a.comm)

SUMI_REGISTER_COLLECTIVE(scatter, "btree", BtreeScatter,
    a.engine, a.root, a.dst, a.src, a.nelems, a.type_size, a.tag, a.cq_id, a.comm)

}
