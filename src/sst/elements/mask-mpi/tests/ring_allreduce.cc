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


#define ssthg_app_name ringallreduce

#include <stdio.h>

#include <mask_mpi.h>
#include <mercury/common/skeleton.h>

// Each rank contributes values[i] = i + rank, so every rank's reduced array
// holds size*i + size*(size-1)/2. test_ring_allreduce.py selects the ring via
// app1.allreduce_alg; nelems does not divide evenly by the rank count, which
// covers remainder chunk sizing.
int main(int argc, char* argv[])
{
    MPI_Init(&argc, &argv);
    int size, rank;
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    const int nelems = 19;
    int* values = new int[nelems];
    for(int i = 0; i < nelems; i++) {
      values[i] = i + rank;
    }
    int base = size * (size - 1) / 2;

    int* recv_values = new int[nelems];
    MPI_Allreduce(values, recv_values, nelems, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    int errors = 0;
    for(int i = 0; i < nelems; ++i) {
      if(recv_values[i] != size * i + base) errors++;
      if(rank == 0) printf("recv_values[%d]=%d\n", i, recv_values[i]);
    }
    printf("allreduce rank=%d errors=%d\n", rank, errors);

    int* irecv_values = new int[nelems];
    MPI_Request req;
    MPI_Iallreduce(values, irecv_values, nelems, MPI_INT, MPI_SUM,
                   MPI_COMM_WORLD, &req);
    MPI_Wait(&req, MPI_STATUS_IGNORE);
    errors = 0;
    for(int i = 0; i < nelems; ++i) {
      if(irecv_values[i] != size * i + base) errors++;
    }
    printf("iallreduce rank=%d errors=%d\n", rank, errors);

    MPI_Finalize();

    return 0;
}
