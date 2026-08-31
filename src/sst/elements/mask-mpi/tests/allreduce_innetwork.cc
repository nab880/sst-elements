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
#include <cstdlib>
#include <cstdio>

#include <mask_mpi.h>
#include <mercury/components/node_base.h>
#include <mercury/components/operating_system_impl.h>
#include <mercury/common/skeleton.h>
#include <mercury/hardware/network/network_message.h>
#include <mercury/operating_system/libraries/library.h>

using namespace SST::Hg;

namespace {

class ManagerSink final : public Library
{
public:
    explicit ManagerSink(OperatingSystemAPI* os) :
        Library("allreduce_manager", SoftwareId(1, static_cast<int>(os->addr())), os)
    {}

    ~ManagerSink() override { os_->unregisterEventLib(this); }

    void incomingRequest(Request* request) override
    {
        auto* message = dynamic_cast<NetworkMessage*>(request);
        received_ = message && message->fromaddr() == 0 && message->toaddr() == 1 &&
                    message->libname() == "allreduce_manager";
        delete request;
        if ( !received_ ) std::abort();
    }

    bool received() const { return received_; }

private:
    bool received_ = false;
};

} // namespace

int main(int argc, char* argv[])
{
    MPI_Init(&argc, &argv);

    int size = 0;
    int rank = 0;
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    const double input = static_cast<double>(rank + 1);
    double result = -1.0;
    MPI_Allreduce(&input, &result, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
    bool pass = size == 4 && std::fabs(result - 4.0) < 1e-12;

    std::array<int32_t, 128> integer_input {};
    std::array<int32_t, 128> integer_result {};
    for ( size_t index = 0; index < integer_input.size(); ++index ) {
        integer_input[index] = static_cast<int32_t>(rank * 1000 + index);
    }
    MPI_Allreduce(integer_input.data(), integer_result.data(),
        static_cast<int>(integer_input.size()), MPI_INT32_T, MPI_MAX, MPI_COMM_WORLD);
    for ( size_t index = 0; index < integer_result.size(); ++index ) {
        pass = pass && integer_result[index] == static_cast<int32_t>((size - 1) * 1000 + index);
    }

    for ( int iteration = 0; iteration < 2; ++iteration ) {
        result = -1.0;
        MPI_Allreduce(&input, &result, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
        pass = pass && std::fabs(result - 10.0) < 1e-12;
    }

    auto* os = OperatingSystemImpl::currentOs();
    ManagerSink manager_sink(os);
    if ( rank == 0 ) {
        auto* message = new NetworkMessage(0, 1, "allreduce_manager", 1, 1, 0, 8,
            false, nullptr, NetworkMessage::smsg {});
        message->putOnWire();
        os->node()->nic()->sendManagerMsg(message);
    }
    ssthg_nanosleep(100);
    pass = pass && (rank != 1 || manager_sink.received());

    printf("Mask-MPI in-network allreduce rank=%d %s\n", rank, pass ? "PASS" : "FAIL");
    fflush(stdout);

    MPI_Finalize();
    return pass ? 0 : 1;
}
