#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../../qemu_plugin/decoder_aarch64.h"

using SST::Quetz::classify_aarch64_insn;
using SST::Quetz::QUETZ_INSN_BRANCH;
using SST::Quetz::QUETZ_INSN_FP_COMPUTE;
using SST::Quetz::QUETZ_INSN_INT_COMPUTE;
using SST::Quetz::QUETZ_INSN_INT_MEM;
using SST::Quetz::QUETZ_INSN_OTHER;
using SST::Quetz::QUETZ_INSN_VEC_COMPUTE;
using SST::Quetz::QUETZ_INSN_VEC_MEM;

TEST_CASE("aarch64 load/store") {
    CHECK(classify_aarch64_insn(0xF9400000u) == QUETZ_INSN_INT_MEM);
    CHECK(classify_aarch64_insn(0x3D800000u) == QUETZ_INSN_VEC_MEM);
}

TEST_CASE("aarch64 groups") {
    CHECK(classify_aarch64_insn(0x11000000u) == QUETZ_INSN_INT_COMPUTE);
    CHECK(classify_aarch64_insn(0x1E200800u) == QUETZ_INSN_FP_COMPUTE);
    CHECK(classify_aarch64_insn(0x14000000u) == QUETZ_INSN_BRANCH);
    CHECK(classify_aarch64_insn(0x4E000800u) == QUETZ_INSN_VEC_COMPUTE);
    CHECK(classify_aarch64_insn(0x00000000u) == QUETZ_INSN_OTHER);
}
