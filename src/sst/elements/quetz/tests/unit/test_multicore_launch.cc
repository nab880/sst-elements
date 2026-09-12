#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../quetz_multicore_launch.h"
using SST::Quetz::multicoreLaunchError;
using Args = std::vector<std::string>;

static Args valid() {
    return {"-M", "raptor-core2,secondary-kernel=/tmp/core1.elf,strict-mmio=on",
            "-smp", "2", "-accel", "tcg,thread=single", "-nographic"};
}
TEST_CASE("two-core launch requires explicit topology and serialized TCG") {
    CHECK(multicoreLaunchError(2, false, valid()) == nullptr);
    CHECK(multicoreLaunchError(2, false, {"--machine=raptor-core2,secondary-kernel=a",
          "-smp=2", "-accel=tcg,thread=single", "-cpu=cfv4e"}) == nullptr);
    CHECK(multicoreLaunchError(1, false, valid()) != nullptr);
    CHECK(multicoreLaunchError(3, false, valid()) != nullptr);
    CHECK(multicoreLaunchError(2, true, valid()) == nullptr);
    for (size_t index : {size_t(0), size_t(2), size_t(4)}) {
        auto args = valid(); args.erase(args.begin() + index, args.begin() + index + 2);
        CHECK(multicoreLaunchError(2, false, args) != nullptr);
    }
}
TEST_CASE("unsupported machine CPU accelerator and topology cannot override contract") {
    for (const std::string machine : {"mcf5208evb", "raptor-core2", "raptor-core2,secondary-kernel=",
         "raptor-core2,secondary-kernel=a,secondary-kernel=b", "raptor-core2,secondary-kernel=a,accel=kvm",
         "raptor-core2,secondary-kernel=a,strict-mmio=off", "raptor-core2,secondary-kernel=a,"}) {
        auto args = valid(); args[1] = machine;
        CHECK(multicoreLaunchError(2, false, args) != nullptr);
    }
    for (const Args& override : {Args{"-M", "mcf5208evb"}, {"-machine", "raptor-core2"},
         {"-cpu", "m5208"}, {"-cpu", "cfv4e,feature=on"}, {"-smp", "2"},
         {"-accel", "tcg,thread=multi"}, {"-readconfig", "x"}, {"-global", "x"}, {"-enable-kvm"}}) {
        auto args = valid(); args.insert(args.end(), override.begin(), override.end());
        CHECK(multicoreLaunchError(2, false, args) != nullptr);
    }
    for (const std::string count : {"1", "3", "2,sockets=2", "2,maxcpus=3", ""}) {
        auto args = valid(); args[3] = count;
        CHECK(multicoreLaunchError(2, false, args) != nullptr);
    }
    for (const std::string accel : {"tcg", "tcg,thread=multi", "kvm"}) {
        auto args = valid(); args[5] = accel;
        CHECK(multicoreLaunchError(2, false, args) != nullptr);
        CHECK(multicoreLaunchError(2, true, args) != nullptr);
    }
}

TEST_CASE("eDMA IRQ routes compose with two-core launch without option injection") {
    auto args = valid();
    args[1] += ",edma-irq=31,edma-error-irq=32";
    CHECK(multicoreLaunchError(2, false, args) == nullptr);
    for (const std::string property : {"edma-irq=64", "edma-irq=-1", "edma-irq=",
         "edma-irq=0x20", "edma-error-irq=999999999999999", "edma-irq=1,edma-irq=2"}) {
        args = valid(); args[1] += ',' + property;
        CHECK(multicoreLaunchError(2, false, args) != nullptr);
    }
}
