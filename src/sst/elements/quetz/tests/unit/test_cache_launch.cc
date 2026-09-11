#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../quetz_cache_launch.h"
using SST::Quetz::windowCacheLaunchError;

TEST_CASE("cache window accepts only the dedicated V4e platform") {
    CHECK(windowCacheLaunchError({"-M", "raptor-core2"}) == nullptr);
    CHECK(windowCacheLaunchError({"-machine", "raptor-core2,strict-mmio=on",
                                 "-display", "none", "-monitor", "none"}) == nullptr);
    CHECK(windowCacheLaunchError({"-machine=raptor-core2", "-cpu", "cfv4e"}) == nullptr);
    CHECK(windowCacheLaunchError({"--machine", "raptor-core2", "--cpu=cfv4e"}) == nullptr);
    CHECK(windowCacheLaunchError({"-serial", "stdio", "-machine", "raptor-core2"}) == nullptr);
}

TEST_CASE("cache window rejects absent or conflicting CPU and machine arguments") {
    CHECK(windowCacheLaunchError({}) != nullptr);
    CHECK(windowCacheLaunchError({"-M", "mcf5208evb", "-cpu", "cfv4e"}) != nullptr);
    CHECK(windowCacheLaunchError({"-M", "raptor-core2", "-cpu", "m5208"}) != nullptr);
    CHECK(windowCacheLaunchError({"-M", "raptor-core2", "-cpu"}) != nullptr);
    CHECK(windowCacheLaunchError({"-M", "raptor-core2", "-machine=none"}) != nullptr);
    CHECK(windowCacheLaunchError({"-M", "none", "-machine=raptor-core2"}) != nullptr);
    CHECK(windowCacheLaunchError({"-M", "raptor-core2", "--cpu=m68040"}) != nullptr);
    CHECK(windowCacheLaunchError({"-M", "raptor-core2,type=mcf5208evb"}) != nullptr);
    CHECK(windowCacheLaunchError({"-M", "raptor-core2,strict-mmio=on,type=none"}) != nullptr);
    CHECK(windowCacheLaunchError({"-M", "raptor-core2,strict-mmio=off"}) != nullptr);
    CHECK(windowCacheLaunchError({"-M", "raptor-core2", "-readconfig", "board.cfg"}) != nullptr);
    CHECK(windowCacheLaunchError({"-M", "raptor-core2", "--readconfig=board.cfg"}) != nullptr);
}
