#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "quetz_balar_flush_range.h"

#include <cstdint>
#include <limits>

using SST::Quetz::BalarFlushRange;
using SST::Quetz::computeBalarFlushRange;

TEST_CASE("Balar flush range covers normal unaligned ranges")
{
    BalarFlushRange range;
    REQUIRE(computeBalarFlushRange(0x1083, 0x100, 64, range));
    CHECK(range.first_line == 0x1080);
    CHECK(range.end == 0x1183);
    CHECK(range.line_count == 5);
}

TEST_CASE("Balar flush range rejects address overflow")
{
    const uint64_t max = std::numeric_limits<uint64_t>::max();
    BalarFlushRange range;
    CHECK_FALSE(computeBalarFlushRange(max - 31, 32, 64, range));
}

TEST_CASE("Balar flush range handles valid addresses near UINT64_MAX")
{
    const uint64_t max = std::numeric_limits<uint64_t>::max();
    BalarFlushRange range;
    REQUIRE(computeBalarFlushRange(max - 191, 128, 64, range));
    CHECK(range.first_line == max - 191);
    CHECK(range.end == max - 63);
    CHECK(range.line_count == 2);
}

TEST_CASE("Balar flush range rejects a cache line crossing UINT64_MAX")
{
    const uint64_t max = std::numeric_limits<uint64_t>::max();
    BalarFlushRange range;
    CHECK_FALSE(computeBalarFlushRange(max - 64, 64, 64, range));
}

TEST_CASE("Balar flush range accepts an empty range and rejects zero line size")
{
    BalarFlushRange range;
    REQUIRE(computeBalarFlushRange(0x1234, 0, 64, range));
    CHECK(range.line_count == 0);
    CHECK_FALSE(computeBalarFlushRange(0x1234, 64, 0, range));
}
