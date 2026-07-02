/**
Copyright 2009-2026 National Technology and Engineering Solutions of Sandia,
LLC (NTESS).  Under the terms of Contract DE-NA-0003525, the U.S. Government
retains certain rights in this software.

Copyright (c) 2009-2026, NTESS
All rights reserved.

This file is part of the SST software package. For license information, see
the LICENSE file in the top level directory of the distribution.
*/

// Tier 1 (mask-mpi flavor): prototype your own collective algorithm in plain
// MPI point-to-point, measured at packet level on your chosen topology, with
// NO hgcc and NO MVAPICH2 -- it runs on mask-mpi, the in-tree MPI, so it builds
// as an ordinary SST element library and runs with just `sst`.
//
// Edit the >>> YOUR ALGORITHM HERE <<< region. The app contributes (rank+1)
// from every rank and asserts the known all-reduce sum n(n+1)/2, so a wrong
// algorithm prints FAIL instead of a meaningless time.
//
// Build:  it is compiled into libmy_collective.so by the mask-mpi Makefile.am.
// Run:    sst test_my_collective.py         (NRANKS=<n> to change rank count)

#define ssthg_app_name my_collective

#include <stdio.h>
#include <mask_mpi.h>
#include <mercury/common/skeleton.h>

/* Reduce one int element: acc += val. Swap in your own op if you like. */
static void reduce_sum(int* acc, const int* val) { *acc += *val; }

/*
 * >>> YOUR ALGORITHM HERE <<<
 *
 * All-reduce (MPI_SUM) the single int `in` across MPI_COMM_WORLD into *out,
 * using only point-to-point messages. Worked example: a naive ring. Each
 * original contribution travels once around the ring; `trav` is the value
 * currently passing through this rank (its own first, then whatever it just
 * received), and `acc` sums each contribution as it arrives. After N-1 steps
 * every rank has added all N contributions exactly once. The
 * even-send-first / odd-recv-first ordering is deadlock-free. Replace with
 * your scheme (recursive doubling, tree, hierarchical, ...).
 */
static void my_allreduce(int in, int* out)
{
  int rank, size;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  int right = (rank + 1) % size;
  int left  = (rank - 1 + size) % size;

  int acc = in;    /* running sum of contributions seen */
  int trav = in;   /* value forwarded this step (received value next step) */
  int recv = 0;
  for (int step = 0; step < size - 1; ++step) {
    if (rank % 2 == 0) {
      MPI_Send(&trav, 1, MPI_INT, right, 0, MPI_COMM_WORLD);
      MPI_Recv(&recv, 1, MPI_INT, left, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    } else {
      MPI_Recv(&recv, 1, MPI_INT, left, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      MPI_Send(&trav, 1, MPI_INT, right, 0, MPI_COMM_WORLD);
    }
    reduce_sum(&acc, &recv);  /* add the arriving contribution */
    trav = recv;              /* forward it onward next step */
  }
  *out = acc;
}
/* >>> END OF YOUR ALGORITHM <<< */

int main(int argc, char** argv)
{
  MPI_Init(&argc, &argv);
  int rank, size;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  int out = -1;
  my_allreduce(rank + 1, &out);

  int expected = size * (size + 1) / 2;
  if (out != expected) {
    printf("FAIL: my_collective rank %d got %d expected %d\n", rank, out, expected);
  } else if (rank == 0) {
    printf("PASS: my_collective (%d ranks, SUM=%d)\n", size, out);
  }

  MPI_Finalize();
  return 0;
}
