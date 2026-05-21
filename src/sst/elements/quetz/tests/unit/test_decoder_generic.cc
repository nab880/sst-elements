#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../../qemu_plugin/decoder_generic.h"

using SST::Quetz::classify_by_size;
using SST::Quetz::QUETZ_INSN_INT_MEM;
using SST::Quetz::QUETZ_INSN_VEC_MEM;

TEST_CASE("generic size fallback") {
    CHECK(classify_by_size(1) == QUETZ_INSN_INT_MEM);
    CHECK(classify_by_size(8) == QUETZ_INSN_INT_MEM);
    CHECK(classify_by_size(15) == QUETZ_INSN_INT_MEM);
    CHECK(classify_by_size(16) == QUETZ_INSN_VEC_MEM);
    CHECK(classify_by_size(32) == QUETZ_INSN_VEC_MEM);
}
