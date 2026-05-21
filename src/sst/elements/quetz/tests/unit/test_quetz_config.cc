#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../../quetz_ipc_types.h"

// Mirrors compute_latency vs detailed_instruction_tracking validation
// in quetz_config.cc (without instantiating SST Params).

using SST::Quetz::QUETZ_INSN_BRANCH;
using SST::Quetz::QUETZ_INSN_FP_COMPUTE;
using SST::Quetz::QUETZ_INSN_INT_COMPUTE;
using SST::Quetz::QUETZ_INSN_VEC_COMPUTE;

static bool needsDetailed(const uint32_t compute_latency[8]) {
    return compute_latency[QUETZ_INSN_INT_COMPUTE] ||
           compute_latency[QUETZ_INSN_FP_COMPUTE] ||
           compute_latency[QUETZ_INSN_VEC_COMPUTE] ||
           compute_latency[QUETZ_INSN_BRANCH];
}

TEST_CASE("compute latency requires detailed tracking") {
    uint32_t lat[8] = {};
    CHECK_FALSE(needsDetailed(lat));
    lat[QUETZ_INSN_INT_COMPUTE] = 4;
    CHECK(needsDetailed(lat));
}

TEST_CASE("detailed tracking gate") {
    uint32_t lat[8] = {};
    lat[QUETZ_INSN_BRANCH] = 2;
    const bool detailed = false;
    const bool invalid = needsDetailed(lat) && !detailed;
    CHECK(invalid);
}
