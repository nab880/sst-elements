#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../../qemu_plugin/decoder_riscv.h"

using SST::Quetz::classify_riscv_insn;
using SST::Quetz::classify_rvc_rv64;
using SST::Quetz::QUETZ_INSN_BRANCH;
using SST::Quetz::QUETZ_INSN_FP_COMPUTE;
using SST::Quetz::QUETZ_INSN_FP_MEM;
using SST::Quetz::QUETZ_INSN_INT_COMPUTE;
using SST::Quetz::QUETZ_INSN_INT_MEM;
using SST::Quetz::QUETZ_INSN_OTHER;
using SST::Quetz::QUETZ_INSN_VEC_COMPUTE;
using SST::Quetz::QUETZ_INSN_VEC_MEM;

static uint32_t enc32(uint32_t opcode, uint32_t funct3, uint32_t rd = 0,
                      uint32_t rs1 = 0, uint32_t rs2 = 0) {
    return (opcode & 0x7Fu) | ((funct3 & 7u) << 12) | ((rd & 31u) << 7) |
           ((rs1 & 31u) << 15) | ((rs2 & 31u) << 20);
}

TEST_CASE("riscv32 opcodes") {
    CHECK(classify_riscv_insn(enc32(0x03, 0)) == QUETZ_INSN_INT_MEM);
    CHECK(classify_riscv_insn(enc32(0x23, 0)) == QUETZ_INSN_INT_MEM);
    CHECK(classify_riscv_insn(enc32(0x07, 2)) == QUETZ_INSN_FP_MEM);
    CHECK(classify_riscv_insn(enc32(0x07, 0)) == QUETZ_INSN_VEC_MEM);
    CHECK(classify_riscv_insn(enc32(0x27, 3)) == QUETZ_INSN_FP_MEM);
    CHECK(classify_riscv_insn(enc32(0x2F, 2)) == QUETZ_INSN_INT_MEM);
    CHECK(classify_riscv_insn(enc32(0x43, 0)) == QUETZ_INSN_FP_COMPUTE);
    CHECK(classify_riscv_insn(enc32(0x53, 0)) == QUETZ_INSN_FP_COMPUTE);
    CHECK(classify_riscv_insn(enc32(0x57, 0)) == QUETZ_INSN_VEC_COMPUTE);
    CHECK(classify_riscv_insn(enc32(0x63, 0)) == QUETZ_INSN_BRANCH);
    CHECK(classify_riscv_insn(enc32(0x67, 0)) == QUETZ_INSN_BRANCH);
    CHECK(classify_riscv_insn(enc32(0x6F, 0)) == QUETZ_INSN_BRANCH);
    CHECK(classify_riscv_insn(enc32(0x13, 0)) == QUETZ_INSN_INT_COMPUTE);
    CHECK(classify_riscv_insn(enc32(0x33, 0)) == QUETZ_INSN_INT_COMPUTE);
    CHECK(classify_riscv_insn(enc32(0x73, 0)) == QUETZ_INSN_OTHER);
    CHECK(classify_riscv_insn(enc32(0x7F, 0)) == QUETZ_INSN_OTHER);
}

TEST_CASE("riscv compressed") {
    CHECK(classify_riscv_insn(0x0000u) == classify_rvc_rv64(0x0000u));
    CHECK(classify_rvc_rv64(0x4000u) == QUETZ_INSN_INT_MEM);
    CHECK(classify_rvc_rv64(0x2000u) == QUETZ_INSN_BRANCH);
}
