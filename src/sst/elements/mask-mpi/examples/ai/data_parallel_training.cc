// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights
// in this software.

#define ssthg_app_name mercury_ai_training

#include <mask_mpi.h>
#include <mercury/common/skeleton.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

using namespace SST::Hg;

namespace {

uint32_t parseUnsigned(
    const char* name, const char* value, uint64_t maximum, bool allowZero = false)
{
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if ( errno != 0 || end == value || *end != '\0' ||
         (!allowZero && parsed == 0) || parsed > maximum ) {
        std::fprintf(stderr, "Mercury AI training received an invalid %s\n", name);
        std::abort();
    }
    return static_cast<uint32_t>(parsed);
}

void verifyClose(int rank, uint32_t epoch, const char* valueName,
    uint32_t index, double actual, double expected)
{
    const double tolerance = 1.0e-10 * std::max(1.0, std::abs(expected));
    if ( !std::isfinite(actual) || std::abs(actual - expected) > tolerance ) {
        std::fprintf(stderr,
            "Mercury AI training verification failed at rank %d epoch %u "
            "for %s[%u]: expected %.12f, got %.12f\n",
            rank, epoch, valueName, index, expected, actual);
        std::abort();
    }
}

void requireMpiSuccess(int result, const char* operation)
{
    if ( result != MPI_SUCCESS ) {
        std::fprintf(stderr, "Mercury AI training %s failed with MPI error %d\n",
            operation, result);
        std::abort();
    }
}

} // namespace

int main(int argc, char** argv)
{
    requireMpiSuccess(MPI_Init(&argc, &argv), "MPI_Init");

    int rank = 0;
    int ranks = 0;
    requireMpiSuccess(MPI_Comm_rank(MPI_COMM_WORLD, &rank), "MPI_Comm_rank");
    requireMpiSuccess(MPI_Comm_size(MPI_COMM_WORLD, &ranks), "MPI_Comm_size");

    if ( argc != 4 ) {
        std::fprintf(stderr,
            "usage: mercury_ai_training scalar|hybrid epochs compute_ns\n");
        std::abort();
    }

    const std::string scenario(argv[1]);
    if ( scenario != "scalar" && scenario != "hybrid" ) {
        std::fprintf(stderr, "Mercury AI training scenario must be scalar or hybrid\n");
        std::abort();
    }
    const uint32_t epochs = parseUnsigned(
        "epoch count", argv[2], std::numeric_limits<uint32_t>::max());
    const uint32_t computeNs = parseUnsigned(
        "compute time", argv[3], std::numeric_limits<unsigned int>::max(), true);
    const uint32_t parameters = scenario == "scalar" ? 1 : 4;
    const bool syncLoss = scenario == "hybrid";
    constexpr double learningRate = 0.5;

    std::array<double, 4> model {};
    std::array<double, 4> gradientSend {};
    std::array<double, 4> gradientRecv {};

    requireMpiSuccess(MPI_Barrier(MPI_COMM_WORLD), "MPI_Barrier");
    const double totalStart = MPI_Wtime();
    double collectiveTotal = 0.0;

    for ( uint32_t epoch = 0; epoch < epochs; ++epoch ) {
        ssthg_nanosleep(computeNs);

        for ( uint32_t parameter = 0; parameter < parameters; ++parameter ) {
            const double offset = static_cast<double>(parameter);
            const double localTarget = static_cast<double>(rank + 1) + offset;
            const double residual = model[parameter] - localTarget;
            gradientSend[parameter] = residual;
            gradientRecv[parameter] = std::numeric_limits<double>::quiet_NaN();
        }

        const double collectiveStart = MPI_Wtime();
        requireMpiSuccess(MPI_Allreduce(gradientSend.data(), gradientRecv.data(),
            static_cast<int>(parameters), MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD),
            "gradient MPI_Allreduce");

        double globalLoss = std::numeric_limits<double>::quiet_NaN();
        if ( syncLoss ) {
            double localLoss = 0.0;
            for ( uint32_t parameter = 0; parameter < parameters; ++parameter ) {
                const double target =
                    static_cast<double>(rank + 1) + static_cast<double>(parameter);
                const double residual = model[parameter] - target;
                localLoss += 0.5 * residual * residual;
            }
            requireMpiSuccess(MPI_Allreduce(&localLoss, &globalLoss, 1,
                MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD), "loss MPI_Allreduce");
        }
        collectiveTotal += MPI_Wtime() - collectiveStart;

        const double epochDecay =
            std::pow(1.0 - learningRate, static_cast<double>(epoch));
        const double meanTargetBase = (static_cast<double>(ranks) + 1.0) / 2.0;
        if ( syncLoss ) {
            double expectedLoss = 0.0;
            for ( uint32_t parameter = 0; parameter < parameters; ++parameter ) {
                const double offset = static_cast<double>(parameter);
                const double expectedModel =
                    (meanTargetBase + offset) * (1.0 - epochDecay);
                for ( int worker = 0; worker < ranks; ++worker ) {
                    const double target = static_cast<double>(worker + 1) + offset;
                    const double residual = expectedModel - target;
                    expectedLoss += 0.5 * residual * residual;
                }
            }
            verifyClose(rank, epoch, "loss", 0, globalLoss, expectedLoss);
        }

        for ( uint32_t parameter = 0; parameter < parameters; ++parameter ) {
            const double meanTarget =
                meanTargetBase + static_cast<double>(parameter);
            const double expectedGradient =
                -static_cast<double>(ranks) * meanTarget * epochDecay;
            verifyClose(rank, epoch, "gradient", parameter,
                gradientRecv[parameter], expectedGradient);
            model[parameter] -= learningRate * gradientRecv[parameter] /
                static_cast<double>(ranks);
        }
    }

    const double totalStop = MPI_Wtime();
    const double decay = std::pow(1.0 - learningRate, static_cast<double>(epochs));
    const double meanTargetBase = (static_cast<double>(ranks) + 1.0) / 2.0;
    for ( uint32_t parameter = 0; parameter < parameters; ++parameter ) {
        const double expected =
            (meanTargetBase + static_cast<double>(parameter)) * (1.0 - decay);
        verifyClose(rank, epochs, "model", parameter, model[parameter], expected);
    }

    std::printf(
        "Mercury AI training verify rank %d parameters %u epochs %u "
        "weight0 %.6f target0 %.6f PASS\n",
        rank, parameters, epochs, model.front(), meanTargetBase);
    std::printf(
        "MercuryAITraining rank %d: ranks %d, epochs %u, parameters %u, "
        "loss-sync %s, step %.6f us, collectives %.6f us\n",
        rank, ranks, epochs, parameters, syncLoss ? "on" : "off",
        (totalStop - totalStart) * 1.0e6 / static_cast<double>(epochs),
        collectiveTotal * 1.0e6 / static_cast<double>(epochs));
    std::fflush(stdout);

    requireMpiSuccess(MPI_Finalize(), "MPI_Finalize");
    return 0;
}
