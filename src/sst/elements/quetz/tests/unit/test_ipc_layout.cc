#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>
#include <sys/mman.h>
#include <unistd.h>

#include "../../quetz_ipc_types.h"

namespace Overlay {
#include "../../qemu-overlay/include/quetz/quetz_ipc_types.h"
constexpr uint32_t Magic = QUETZ_SHM_MAGIC;
}
#undef QUETZ_SHM_MAGIC
#undef QUETZ_LOCAL_RAM_BYTES
#undef QUETZ_MAX_MMIO_VCORES
#undef QUETZ_MAX_IRQ_LINES

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

// The IRQ mailbox layout is shared with the QEMU overlay's C mirror
// (qemu-overlay/include/quetz/quetz_ipc_types.h) — these pins
// catch a drift between the two copies.
TEST_CASE("QuetzIrqSlot layout") {
    using SST::Quetz::QuetzIrqSlot;
    using SST::Quetz::QUETZ_MAX_IRQ_LINES;
    CHECK(sizeof(QuetzIrqSlot) == 8);
    CHECK(offsetof(QuetzIrqSlot, seq) == 0);
    CHECK(offsetof(QuetzIrqSlot, level) == 4);
    CHECK(QUETZ_MAX_IRQ_LINES == 64);
    CHECK(offsetof(QuetzSharedData, irq_slot) ==
          offsetof(QuetzSharedData, mmio_req) +
          sizeof(QuetzSharedData::mmio_req));
}

TEST_CASE("C and C++ IPC layouts include identical reset and RAM metadata") {
    CHECK(sizeof(QuetzSharedData) == sizeof(Overlay::QuetzSharedData));
    CHECK(SST::Quetz::QUETZ_SHM_MAGIC == Overlay::Magic);
    CHECK(offsetof(QuetzSharedData, cpu_reset_epoch) ==
          offsetof(Overlay::QuetzSharedData, cpu_reset_epoch));
    CHECK(offsetof(QuetzSharedData, local_ram_offset) ==
          offsetof(Overlay::QuetzSharedData, local_ram_offset));
    CHECK(offsetof(QuetzSharedData, local_ram_storage) ==
          offsetof(Overlay::QuetzSharedData, local_ram_storage));
    CHECK(offsetof(QuetzSharedData, magic) == offsetof(Overlay::QuetzSharedData, magic));
}

TEST_CASE("native RAM keeps one offset across different mmap alignment residues") {
    const size_t page = size_t(sysconf(_SC_PAGESIZE));
    if (page >= 65536) return;
    const size_t shared_offset = 128;
    const size_t length = (shared_offset + sizeof(QuetzSharedData) + page - 1) & ~(page - 1);
    const size_t gap = (length + 65535) & ~size_t(65535);
    const size_t reserved_size = 2 * gap + 3 * 65536;
    struct Resources {
        FILE* file = nullptr;
        void* mapping = MAP_FAILED;
        size_t size = 0;
        ~Resources() {
            if (mapping != MAP_FAILED) munmap(mapping, size);
            if (file) fclose(file);
        }
    } resources;
    resources.file = tmpfile();
    REQUIRE(resources.file != nullptr);
    REQUIRE(ftruncate(fileno(resources.file), length) == 0);
    resources.size = reserved_size;
    resources.mapping = mmap(nullptr, reserved_size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(resources.mapping != MAP_FAILED);
    const uintptr_t first = (uintptr_t(resources.mapping) + 65535) & ~uintptr_t(65535);
    const uintptr_t second = first + gap + page;
    REQUIRE(mmap(reinterpret_cast<void*>(first), length, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_FIXED, fileno(resources.file), 0) != MAP_FAILED);
    REQUIRE(mmap(reinterpret_cast<void*>(second), length, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_FIXED, fileno(resources.file), 0) != MAP_FAILED);
    auto* sst = new(reinterpret_cast<void*>(first + shared_offset)) QuetzSharedData{};
    auto* qemu = new(reinterpret_cast<void*>(second + shared_offset)) Overlay::QuetzSharedData;
    SST::Quetz::quetzInitializeLocalRam(sst);
    CHECK((uintptr_t(sst) & 65535) != (uintptr_t(qemu) & 65535));
    for (unsigned bank = 0; bank < 2; ++bank) {
        auto* a = SST::Quetz::quetzLocalRam(sst, bank);
        auto* b = Overlay::quetz_local_ram(qemu, bank);
        REQUIRE(a != nullptr); REQUIRE(b != nullptr);
        CHECK(a - reinterpret_cast<uint8_t*>(sst) == b - reinterpret_cast<uint8_t*>(qemu));
        CHECK((uintptr_t(a) & 65535) == 0);
        CHECK((uintptr_t(b) & (page - 1)) == 0);
        a[0] = uint8_t(0x21 + bank);
        b[65535] = uint8_t(0x81 + bank);
        CHECK(b[0] == 0x21 + bank);
        CHECK(a[65535] == 0x81 + bank);
    }
    CHECK(SST::Quetz::quetzLocalRam(sst, 2) == nullptr);
    CHECK(Overlay::quetz_local_ram(qemu, 2) == nullptr);
    sst->local_ram_offset = 0;
    CHECK(SST::Quetz::quetzLocalRam(sst, 0) == nullptr);
    CHECK(Overlay::quetz_local_ram(qemu, 0) == nullptr);
    sst->local_ram_offset = UINT32_MAX;
    CHECK(SST::Quetz::quetzLocalRam(sst, 0) == nullptr);
    CHECK(Overlay::quetz_local_ram(qemu, 0) == nullptr);
}
