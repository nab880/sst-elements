#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <string>

#include "sst_stubs.h"

// Mirrors MemRegionTable::findHandler first-match semantics.

TEST_CASE("region first-match ordering") {
    quetz_unit::RegionRange regions[] = {
        { "uart0",   0x10000000, 0x10000FFF },
        { "sub_ram", 0x00000000, 0x7FFFFFFF },
    };
    const auto* hit = quetz_unit::findFirstMatch(regions, 2, 0x10000008);
    REQUIRE(hit != nullptr);
    CHECK(std::string(hit->name) == "uart0");

    hit = quetz_unit::findFirstMatch(regions, 2, 0x00001000);
    REQUIRE(hit != nullptr);
    CHECK(std::string(hit->name) == "sub_ram");
}

TEST_CASE("region boundaries") {
    quetz_unit::RegionRange regions[] = {
        { "a", 0x1000, 0x2000 },
    };
    CHECK(quetz_unit::findFirstMatch(regions, 1, 0x1000) != nullptr);
    CHECK(quetz_unit::findFirstMatch(regions, 1, 0x2000) != nullptr);
    CHECK(quetz_unit::findFirstMatch(regions, 1, 0x0FFF) == nullptr);
    CHECK(quetz_unit::findFirstMatch(regions, 1, 0x2001) == nullptr);
}
