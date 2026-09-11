#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../qemu_plugin/coldfire_cache_ops.h"
using SST::Quetz::decodeColdFireCacheOp;

TEST_CASE("MOVEC decodes big-endian bytes and each D/A source") {
    for (unsigned source = 0; source < 16; ++source) {
        for (uint16_t control : {0x002, 0x004, 0x005, 0x006, 0x007}) {
            const uint8_t bytes[] = {0x4e, 0x7b, uint8_t(source << 4),
                                    uint8_t(control)};
            const auto op = decodeColdFireCacheOp(bytes, sizeof(bytes));
            REQUIRE(op.valid);
            CHECK(op.kind == 0);
            CHECK(op.control == control);
            CHECK(op.source == source);
        }
    }
}

TEST_CASE("cache decoder excludes unrelated and partial instructions") {
    const uint8_t vbr[] = {0x4e, 0x7b, 0x08, 0x01};
    const uint8_t read_cacr[] = {0x4e, 0x7a, 0x00, 0x02};
    const uint8_t truncated[] = {0x4e, 0x7b, 0x00};
    const uint8_t nop[] = {0x4e, 0x71};
    CHECK_FALSE(decodeColdFireCacheOp(vbr, sizeof(vbr)).valid);
    CHECK_FALSE(decodeColdFireCacheOp(read_cacr, sizeof(read_cacr)).valid);
    CHECK_FALSE(decodeColdFireCacheOp(truncated, sizeof(truncated)).valid);
    CHECK_FALSE(decodeColdFireCacheOp(nop, sizeof(nop)).valid);
    CHECK_FALSE(decodeColdFireCacheOp(nullptr, 0).valid);
}

TEST_CASE("CPUSHL retains cache selector and set/way source") {
    for (uint8_t selection : {0x40, 0x80, 0xc0}) {
        for (unsigned source = 0; source < 8; ++source) {
            const uint8_t bytes[] = {0xf4, uint8_t(0x28 | selection | source)};
            const auto op = decodeColdFireCacheOp(bytes, sizeof(bytes));
            REQUIRE(op.valid);
            CHECK(op.kind == 1);
            CHECK(op.instruction == uint16_t(0xf400 | bytes[1]));
            CHECK(op.source == source + 8);
        }
    }
    const uint8_t cinv[] = {0xf4, 0x48};
    CHECK_FALSE(decodeColdFireCacheOp(cinv, sizeof(cinv)).valid);
}
