// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#include "quetz_config_manager.h"

#include <stdint.h>
#include <vector>

namespace SST {
namespace Quetz {

namespace {

RegionHandlerPreset filtered(const char* start, const char* end) {
    return {
        "quetz.FilteredRegionHandler",
        { { "start", start }, { "end", end } }
    };
}

RegionHandlerPreset uart0() {
    return {
        "quetz.UartRegionHandler",
        { { "start", "0x10000000" }, { "end", "0x10000FFF" }, { "tx_offset", "0" } }
    };
}

} // namespace

const std::vector<QuetzPlatformProfile>& quetzPlatformRegistry() {
    static const std::vector<QuetzPlatformProfile> kProfiles = {
        {
            "riscv64_virt",
            "qemu-system-riscv64 virt machine with sub_ram filtered",
            {
                { "qemu",               "qemu-system-riscv64" },
                { "system_mode",        "1" },
                { "system_mode_loader", "-kernel" },
                { "qemu_args",          "-machine virt -nographic -bios none" },
                { "isa",                "rv64gc" },
            },
            { filtered("0x0", "0x7FFFFFFF") },
        },
        {
            "riscv64_virt_uart",
            "qemu-system-riscv64 virt with UART capture + sub_ram filter",
            {
                { "qemu",               "qemu-system-riscv64" },
                { "system_mode",        "1" },
                { "system_mode_loader", "-kernel" },
                { "qemu_args",          "-machine virt -nographic -bios none" },
                { "isa",                "rv64gc" },
            },
            { uart0(), filtered("0x0", "0x7FFFFFFF") },
        },
        {
            "arm_m7",
            "qemu-system-arm Cortex-M7 (mps2-an500) with peripheral filter",
            {
                { "qemu",               "qemu-system-arm" },
                { "system_mode",        "1" },
                { "system_mode_loader", "-kernel" },
                { "qemu_args",
                  "-machine mps2-an500 -nographic "
                  "-semihosting-config enable=on,target=native" },
                { "isa",                "aarch32-m7" },
            },
            { filtered("0x40000000", "0xFFFFFFFF") },
        },
        {
            "x86_baremetal",
            "qemu-system-i386 raw bare-metal (no region handlers)",
            {
                { "qemu",               "qemu-system-i386" },
                { "system_mode",        "1" },
                { "system_mode_loader", "-kernel" },
                { "isa",                "x86" },
            },
            {},
        },
        {
            "riscv64_usermode",
            "qemu-riscv64 user-mode",
            {
                { "qemu",               "qemu-riscv64" },
                { "system_mode",        "0" },
                { "isa",                "rv64gc" },
            },
            {},
        },
        {
            "aarch64_usermode",
            "qemu-aarch64 user-mode",
            {
                { "qemu",               "qemu-aarch64" },
                { "system_mode",        "0" },
                { "isa",                "aarch64" },
            },
            {},
        },
        {
            "x86_64_usermode",
            "qemu-x86_64 user-mode",
            {
                { "qemu",               "qemu-x86_64" },
                { "system_mode",        "0" },
                { "isa",                "x86_64" },
            },
            {},
        },
    };
    return kProfiles;
}

} // namespace Quetz
} // namespace SST
