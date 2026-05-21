#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "sst_stubs.h"

// Mirrors MemRequestEmitter::slotsNeeded (quetz_mem_issue.cc).

TEST_CASE("cache line slots") {
    const uint64_t line = 64;
    CHECK(quetz_unit::slotsNeeded(0, 1, line) == 1);
    CHECK(quetz_unit::slotsNeeded(0, 64, line) == 1);
    CHECK(quetz_unit::slotsNeeded(60, 8, line) == 2);
    CHECK(quetz_unit::slotsNeeded(0, 128, line) == 2);
    CHECK(quetz_unit::slotsNeeded(0, 0, line) == 1);
}

TEST_CASE("split extra requests") {
    const uint32_t parts = quetz_unit::slotsNeeded(60, 16, 64);
    CHECK(parts == 2);
    CHECK(parts - 1 == 1);
}
