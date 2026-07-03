// Unit tests for the saturating int16 scale/offset math (quetz_scale_offset.h)
// that quetz.ScaleOffsetKernel runs on the device. The in-sim firmware test
// checks one parameter set end to end; these pin down the saturation edges.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <cstdint>

#include "../../quetz_scale_offset.h"

using SST::Quetz::quetz_scale_offset_sat16;

TEST_CASE("identity: scale=1 offset=0") {
    for (int32_t s : {-32768, -1, 0, 1, 32767}) {
        CHECK(quetz_scale_offset_sat16((int16_t)s, 1, 0) == (int16_t)s);
    }
}

TEST_CASE("basic affine transform") {
    CHECK(quetz_scale_offset_sat16(100, 3, -25) == 275);
    CHECK(quetz_scale_offset_sat16(-100, 3, -25) == -325);
    CHECK(quetz_scale_offset_sat16(0, 3, -25) == -25);
    CHECK(quetz_scale_offset_sat16(7, -2, 10) == -4);
}

TEST_CASE("saturates high") {
    CHECK(quetz_scale_offset_sat16(32767, 2, 0) == 32767);
    CHECK(quetz_scale_offset_sat16(20000, 2, 0) == 32767);
    CHECK(quetz_scale_offset_sat16(32767, 1, 1) == 32767);
    CHECK(quetz_scale_offset_sat16(-32768, -1, 0) == 32767);  // 32768 clamps
}

TEST_CASE("saturates low") {
    CHECK(quetz_scale_offset_sat16(-32768, 2, 0) == -32768);
    CHECK(quetz_scale_offset_sat16(-20000, 2, 0) == -32768);
    CHECK(quetz_scale_offset_sat16(-32768, 1, -1) == -32768);
}

TEST_CASE("32-bit intermediate: no wrap at int16 extremes") {
    // (-32768) * (-32768) = 2^30 — far beyond int16 but fine in int32.
    CHECK(quetz_scale_offset_sat16(-32768, -32768, 0) == 32767);
    CHECK(quetz_scale_offset_sat16(32767, 32767, 32767) == 32767);
    CHECK(quetz_scale_offset_sat16(32767, -32768, -32768) == -32768);
}

TEST_CASE("scale or offset of zero") {
    CHECK(quetz_scale_offset_sat16(12345, 0, 0) == 0);
    CHECK(quetz_scale_offset_sat16(12345, 0, -7) == -7);
}
