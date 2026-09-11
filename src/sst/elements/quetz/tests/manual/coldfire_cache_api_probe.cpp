// Standalone QEMU plugin for the public cache instruction/API verification.
#include <glib.h>
extern "C" {
#include "qemu-plugin.h"
}
#include "coldfire_cache_ops.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

using SST::Quetz::decodeColdFireCacheOp;
extern "C" { QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION; }
static qemu_plugin_register* registers[16]{};
static GByteArray* register_bytes;

static void vcpu_init(qemu_plugin_id_t, unsigned)
{
    GArray* list = qemu_plugin_get_registers();
    for (guint i = 0; i < list->len; ++i) {
        const auto reg = g_array_index(list, qemu_plugin_reg_descriptor, i);
        fprintf(stderr, "REG %s %s\n", reg.name, reg.feature ? reg.feature : "");
        if (reg.name[0] == 'd' && reg.name[1] >= '0' && reg.name[1] <= '7' &&
            reg.name[2] == '\0')
            registers[reg.name[1] - '0'] = reg.handle;
        if (reg.name[0] == 'a' && reg.name[1] >= '0' && reg.name[1] <= '7' &&
            reg.name[2] == '\0')
            registers[8 + reg.name[1] - '0'] = reg.handle;
        if (strcmp(reg.name, "fp") == 0) registers[14] = reg.handle;
        if (strcmp(reg.name, "sp") == 0) registers[15] = reg.handle;
    }
    for (const auto* reg : registers) if (!reg) exit(2);
    g_array_free(list, true);
    register_bytes = g_byte_array_new();
}

static void cache_exec(unsigned, void* userdata)
{
    const uint32_t encoding = uint32_t(uintptr_t(userdata));
    const uint8_t bytes[] = {uint8_t(encoding >> 24), uint8_t(encoding >> 16),
                            uint8_t(encoding >> 8), uint8_t(encoding)};
    const auto op = decodeColdFireCacheOp(bytes, sizeof(bytes));
    g_byte_array_set_size(register_bytes, 0);
    if (qemu_plugin_read_register(registers[op.source], register_bytes) != 4 ||
        register_bytes->len != 4) exit(3);
    const auto* data = register_bytes->data;
    const uint32_t value = (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) |
                           (uint32_t(data[2]) << 8) | data[3];
    fprintf(stderr, "OP kind=%u control=%03x source=%u value=%08x instruction=%04x\n",
            op.kind, op.control, op.source, value, op.instruction);
}

static void translate(qemu_plugin_id_t, qemu_plugin_tb* tb)
{
    for (size_t i = 0; i < qemu_plugin_tb_n_insns(tb); ++i) {
        auto* insn = qemu_plugin_tb_get_insn(tb, i);
        uint8_t bytes[4]{};
        const size_t size = qemu_plugin_insn_data(insn, bytes, sizeof(bytes));
        if (!decodeColdFireCacheOp(bytes, size).valid) continue;
        const uint32_t encoding = (uint32_t(bytes[0]) << 24) |
            (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) | bytes[3];
        qemu_plugin_register_vcpu_insn_exec_cb(insn, cache_exec,
            QEMU_PLUGIN_CB_R_REGS, reinterpret_cast<void*>(uintptr_t(encoding)));
    }
}

extern "C" QEMU_PLUGIN_EXPORT int qemu_plugin_install(
    qemu_plugin_id_t id, const qemu_info_t*, int, char**)
{
    qemu_plugin_register_vcpu_init_cb(id, vcpu_init);
    qemu_plugin_register_vcpu_tb_trans_cb(id, translate);
    return 0;
}
