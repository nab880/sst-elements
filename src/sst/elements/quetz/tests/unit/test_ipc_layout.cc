#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <cstddef>
#include <cstdint>

#include "../../quetz_ipc_types.h"

using SST::Quetz::QuetzCommand;
using SST::Quetz::QUETZ_CMD_DATA_BYTES;
using SST::Quetz::QuetzInsnClass;
using SST::Quetz::QuetzSharedData;
using SST::Quetz::QuetzShmemCmd;

TEST_CASE("QuetzCommand layout") {
    CHECK(sizeof(QuetzCommand) >= 48);
    CHECK(alignof(QuetzCommand) >= 4);
    CHECK(offsetof(QuetzCommand, cmd) == 0);
    CHECK(offsetof(QuetzCommand, size) == 4);
    CHECK(offsetof(QuetzCommand, pc) == 8);
    CHECK(offsetof(QuetzCommand, addr) == 16);
    CHECK(offsetof(QuetzCommand, insn_class) == 24);
    CHECK(offsetof(QuetzCommand, data) == 32);
    CHECK(sizeof(QuetzCommand::data) == QUETZ_CMD_DATA_BYTES);
}

TEST_CASE("IPC enums") {
    CHECK(QuetzShmemCmd::QUETZ_CMD_NOP == 0);
    CHECK(QuetzShmemCmd::QUETZ_CMD_READ == 1);
    CHECK(QuetzShmemCmd::QUETZ_CMD_WRITE == 2);
    CHECK(QuetzShmemCmd::QUETZ_CMD_EXIT == 3);
    CHECK(QuetzInsnClass::QUETZ_INSN_CLASS_COUNT == 8);
}

TEST_CASE("QuetzSharedData layout") {
    CHECK(sizeof(QuetzSharedData) >= 24);
    CHECK(offsetof(QuetzSharedData, numCores) == 0);
    CHECK(offsetof(QuetzSharedData, mmio_slot) == 32);
    CHECK(QuetzShmemCmd::QUETZ_CMD_MMIO_READ_REQ == 4);
    CHECK(QuetzShmemCmd::QUETZ_CMD_MMIO_WRITE_REQ == 5);
}
