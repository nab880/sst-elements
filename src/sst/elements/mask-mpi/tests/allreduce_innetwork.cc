/**
Copyright 2009-2026 National Technology and Engineering Solutions of Sandia,
LLC (NTESS).  Under the terms of Contract DE-NA-0003525 with NTESS, the U.S.
Government retains certain rights in this software.

Copyright (c) 2009-2026, NTESS
All rights reserved.
*/

#define ssthg_app_name allreduce_innetwork

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <mask_mpi.h>
#include <mercury/common/skeleton.h>

int main(int argc, char* argv[])
{
    MPI_Init(&argc, &argv);

    int size = 0;
    int rank = 0;
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    const bool fallback = argc > 1 && std::strcmp(argv[1], "fallback") == 0;
    const double input = static_cast<double>(rank + 1);
    double result = -1.0;
    for ( int iteration = 0; iteration < 2; ++iteration ) {
        result = -1.0;
        MPI_Allreduce(&input, &result, 1, MPI_DOUBLE,
            fallback ? MPI_MAX : MPI_SUM, MPI_COMM_WORLD);
    }

    std::array<int32_t, 128> integer_input {};
    std::array<int32_t, 128> integer_result {};
    for ( size_t index = 0; index < integer_input.size(); ++index ) {
        integer_input[index] = static_cast<int32_t>(rank * 1000 + index);
    }
    MPI_Allreduce(integer_input.data(), integer_result.data(),
        static_cast<int>(integer_input.size()), MPI_INT32_T, MPI_MAX, MPI_COMM_WORLD);

    const double expected = fallback ? 4.0 : 10.0;
    bool pass = size == 4 && std::fabs(result - expected) < 1e-12;
    for ( size_t index = 0; index < integer_result.size(); ++index ) {
        pass = pass && integer_result[index] ==
            static_cast<int32_t>((size - 1) * 1000 + index);
    }

    printf(
        "Mask-MPI in-network allreduce rank=%d mode=%s input=%.1f result=%.1f %s\n",
        rank, fallback ? "fallback" : "supported", input, result,
        pass ? "PASS" : "FAIL");
    fflush(stdout);

    MPI_Finalize();
    return pass ? 0 : 1;
}
