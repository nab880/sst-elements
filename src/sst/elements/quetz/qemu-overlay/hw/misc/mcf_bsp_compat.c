/*
 * mcf_bsp_compat.c -- profile-driven ColdFire BSP initialization support.
 *
 * This intentionally models register behavior, not peripheral timing.  Sparse
 * blocks from a JSON profile are overlaid on QEMU's otherwise-unassigned
 * MCF5208 IPS ranges.  Unknown offsets remain read-as-zero/write-ignored and
 * every access can be written to a JSONL discovery log.
 */

#include "qemu/osdep.h"
#include "hw/core/cpu.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "qapi/error.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qlist.h"
#include "qapi/qmp/qnum.h"
#include "qapi/qmp/qstring.h"
#include "qemu/error-report.h"
#include "qemu/module.h"

#define TYPE_MCF_BSP_COMPAT "mcf-bsp-compat"
OBJECT_DECLARE_SIMPLE_TYPE(McfBspCompatState, MCF_BSP_COMPAT)

typedef struct McfBspTrigger {
    uint64_t mask;
    uint64_t equals;
    uint64_t target_offset;
    uint64_t set_bits;
    uint64_t clear_bits;
} McfBspTrigger;

typedef struct McfBspRegister {
    char *name;
    uint64_t offset;
    unsigned width;
    uint64_t value;
    uint64_t writable_mask;
    uint64_t read_set;
    uint64_t read_clear;
    uint64_t w1c;
    uint64_t w1s;
    GArray *read_sequence;       /* uint64_t entries; last value is sticky */
    size_t read_sequence_pos;
    GArray *triggers;            /* McfBspTrigger entries */
    bool width_mismatch_reported;
} McfBspRegister;

typedef struct McfBspBlock {
    struct McfBspCompatState *owner;
    char *name;
    uint64_t base;
    uint64_t size;
    MemoryRegion region;
    bool mapped;
    GPtrArray *registers;        /* McfBspRegister* */
} McfBspBlock;

struct McfBspCompatState {
    DeviceState parent_obj;
    char *target;
    char *profile;
    char *log_path;
    bool discover;
    FILE *log_file;
    GPtrArray *blocks;           /* McfBspBlock* */
};

typedef struct MCFBlockDesc {
    const char *name;
    uint64_t base;
    uint64_t size;
} MCFBlockDesc;

/* Only ranges QEMU 9.2.1 leaves unimplemented.  Native FEC, INTC, UART,
 * PIT, RCM, and SDRAMC ranges are deliberately absent. */
static const MCFBlockDesc mcf5208_blocks[] = {
    { "scm",      0xfc000000, 0x4000 },
    { "xbs",      0xfc004000, 0x4000 },
    { "fbcs",     0xfc008000, 0x4000 },
    { "scm2",     0xfc040000, 0x4000 },
    { "edma",     0xfc044000, 0x4000 },
    { "i2c",      0xfc058000, 0x4000 },
    { "qspi",     0xfc05c000, 0x4000 },
    { "dtim0",    0xfc070000, 0x4000 },
    { "dtim1",    0xfc074000, 0x4000 },
    { "eport",    0xfc088000, 0x4000 },
    { "watchdog", 0xfc08c000, 0x4000 },
    { "pll",      0xfc090000, 0x4000 },
    { "wtm",      0xfc098000, 0x4000 },
    { "gpio",     0xfc0a4000, 0x4000 },
};

static uint64_t width_mask(unsigned width)
{
    return width >= 8 ? UINT64_MAX : ((1ULL << (width * 8)) - 1);
}

static uint64_t bsp_pc(void)
{
    CPUState *cpu = current_cpu;

    if (!cpu || !cpu->cc || !cpu->cc->get_pc) {
        return 0;
    }
    return cpu->cc->get_pc(cpu);
}

static bool obj_u64(QObject *obj, uint64_t *value)
{
    QNum *num;
    QString *str;
    char *end = NULL;
    unsigned long long parsed;

    if (!obj) {
        return false;
    }
    num = qobject_to(QNum, obj);
    if (num) {
        return qnum_get_try_uint(num, value);
    }
    str = qobject_to(QString, obj);
    if (!str) {
        return false;
    }
    errno = 0;
    parsed = strtoull(qstring_get_str(str), &end, 0);
    if (errno || !end || *end) {
        return false;
    }
    *value = parsed;
    return true;
}

static bool dict_u64(QDict *dict, const char *key, uint64_t def,
                     uint64_t *value, Error **errp)
{
    QObject *obj = qdict_get(dict, key);

    if (!obj) {
        *value = def;
        return true;
    }
    if (!obj_u64(obj, value)) {
        error_setg(errp, "mcf-bsp-compat: '%s' must be an unsigned integer "
                   "or an integer string", key);
        return false;
    }
    return true;
}

static const MCFBlockDesc *target_block(const char *name, uint64_t base,
                                        uint64_t size)
{
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(mcf5208_blocks); i++) {
        const MCFBlockDesc *d = &mcf5208_blocks[i];
        if (!strcmp(name, d->name) && base == d->base && size <= d->size) {
            return d;
        }
    }
    return NULL;
}

static void bsp_register_free(gpointer opaque)
{
    McfBspRegister *reg = opaque;

    g_free(reg->name);
    if (reg->read_sequence) {
        g_array_unref(reg->read_sequence);
    }
    if (reg->triggers) {
        g_array_unref(reg->triggers);
    }
    g_free(reg);
}

static void bsp_block_free(gpointer opaque)
{
    McfBspBlock *block = opaque;

    if (block->mapped) {
        memory_region_del_subregion(get_system_memory(), &block->region);
        object_unparent(OBJECT(&block->region));
    }
    g_clear_pointer(&block->registers, g_ptr_array_unref);
    g_free(block->name);
    g_free(block);
}

static McfBspRegister *find_register(McfBspBlock *block, uint64_t offset,
                                     unsigned width)
{
    guint i;

    for (i = 0; i < block->registers->len; i++) {
        McfBspRegister *reg = g_ptr_array_index(block->registers, i);
        if (reg->offset == offset && reg->width == width) {
            return reg;
        }
    }
    return NULL;
}

static McfBspRegister *find_register_offset(McfBspBlock *block,
                                            uint64_t offset)
{
    guint i;

    for (i = 0; i < block->registers->len; i++) {
        McfBspRegister *reg = g_ptr_array_index(block->registers, i);
        if (reg->offset == offset) {
            return reg;
        }
    }
    return NULL;
}

static void report_width_mismatch(McfBspBlock *block, uint64_t offset,
                                  unsigned width)
{
    McfBspRegister *reg = find_register_offset(block, offset);

    if (!reg || reg->width_mismatch_reported) {
        return;
    }
    reg->width_mismatch_reported = true;
    warn_report("mcf-bsp-compat: width mismatch for %s.%s at 0x%" PRIx64
                ": profile width=%u, guest width=%u; access remains RAZ/WI",
                block->name, reg->name, block->base + offset,
                reg->width, width);
}

static void bsp_log_access(McfBspBlock *block, const char *op,
                           uint64_t offset, unsigned size, uint64_t value,
                           bool known)
{
    McfBspCompatState *s = block->owner;

    if (!s->log_file) {
        return;
    }
    fprintf(s->log_file,
            "{\"pc\":\"0x%016" PRIx64 "\",\"address\":\"0x%016" PRIx64
            "\",\"block\":\"%s\",\"offset\":\"0x%" PRIx64
            "\",\"size\":%u,\"op\":\"%s\",\"value\":\"0x%016" PRIx64
            "\",\"known\":%s}\n",
            bsp_pc(), block->base + offset, block->name, offset, size, op,
            value, known ? "true" : "false");
    fflush(s->log_file);
}

static uint64_t bsp_read(void *opaque, hwaddr offset, unsigned size)
{
    McfBspBlock *block = opaque;
    McfBspRegister *reg = find_register(block, offset, size);
    uint64_t value = 0;

    if (!reg) {
        report_width_mismatch(block, offset, size);
    }
    if (reg) {
        value = reg->value;
        if (reg->read_sequence && reg->read_sequence->len) {
            size_t pos = MIN(reg->read_sequence_pos,
                             reg->read_sequence->len - 1);
            value = g_array_index(reg->read_sequence, uint64_t, pos);
            if (reg->read_sequence_pos + 1 < reg->read_sequence->len) {
                reg->read_sequence_pos++;
            }
        }
        value |= reg->read_set;
        value &= ~reg->read_clear;
        value &= width_mask(size);
    }
    bsp_log_access(block, "read", offset, size, value, reg != NULL);
    return value;
}

static void bsp_write(void *opaque, hwaddr offset, uint64_t input,
                      unsigned size)
{
    McfBspBlock *block = opaque;
    McfBspRegister *reg = find_register(block, offset, size);
    uint64_t mask = width_mask(size);
    guint i;

    input &= mask;
    bsp_log_access(block, "write", offset, size, input, reg != NULL);
    if (!reg) {
        report_width_mismatch(block, offset, size);
        return;
    }

    reg->value = (reg->value & ~reg->writable_mask) |
                 (input & reg->writable_mask);
    reg->value &= ~(input & reg->w1c);
    reg->value |= input & reg->w1s;
    reg->value &= mask;

    for (i = 0; i < reg->triggers->len; i++) {
        McfBspTrigger *trigger = &g_array_index(reg->triggers,
                                                McfBspTrigger, i);
        McfBspRegister *target;
        if ((input & trigger->mask) != trigger->equals) {
            continue;
        }
        target = find_register_offset(block, trigger->target_offset);
        if (target) {
            target->value &= ~trigger->clear_bits;
            target->value |= trigger->set_bits;
            target->value &= width_mask(target->width);
        }
    }
}

static const MemoryRegionOps bsp_ops = {
    .read = bsp_read,
    .write = bsp_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl  = { .min_access_size = 1, .max_access_size = 4 },
};

static bool parse_sequence(QDict *dict, McfBspRegister *reg, Error **errp)
{
    QList *list = qdict_get_qlist(dict, "read_sequence");
    const QListEntry *entry;

    if (!list) {
        return true;
    }
    QLIST_FOREACH_ENTRY(list, entry) {
        uint64_t value;
        if (!obj_u64(qlist_entry_obj(entry), &value)) {
            error_setg(errp, "mcf-bsp-compat: read_sequence values must be "
                       "unsigned integers or integer strings");
            return false;
        }
        g_array_append_val(reg->read_sequence, value);
    }
    return true;
}

static bool parse_triggers(QDict *dict, McfBspRegister *reg, Error **errp)
{
    QList *list = qdict_get_qlist(dict, "on_write");
    const QListEntry *entry;

    if (!list) {
        return true;
    }
    QLIST_FOREACH_ENTRY(list, entry) {
        QDict *td = qobject_to(QDict, qlist_entry_obj(entry));
        McfBspTrigger trigger = { 0 };

        if (!td ||
            !dict_u64(td, "mask", UINT64_MAX, &trigger.mask, errp) ||
            !dict_u64(td, "equals", 0, &trigger.equals, errp) ||
            !dict_u64(td, "target", 0, &trigger.target_offset, errp) ||
            !dict_u64(td, "set", 0, &trigger.set_bits, errp) ||
            !dict_u64(td, "clear", 0, &trigger.clear_bits, errp)) {
            if (td == NULL && errp && *errp == NULL) {
                error_setg(errp, "mcf-bsp-compat: on_write entry must be an object");
            }
            return false;
        }
        g_array_append_val(reg->triggers, trigger);
    }
    return true;
}

static bool parse_register(McfBspBlock *block, QDict *dict, Error **errp)
{
    McfBspRegister *reg = g_new0(McfBspRegister, 1);
    uint64_t width = 0;
    const char *name = qdict_get_try_str(dict, "name");

    reg->name = g_strdup(name ? name : "unnamed");
    reg->read_sequence = g_array_new(false, false, sizeof(uint64_t));
    reg->triggers = g_array_new(false, false, sizeof(McfBspTrigger));

    if (!dict_u64(dict, "offset", UINT64_MAX, &reg->offset, errp) ||
        !dict_u64(dict, "width", 0, &width, errp) ||
        !dict_u64(dict, "reset", 0, &reg->value, errp) ||
        !dict_u64(dict, "writable_mask", 0, &reg->writable_mask, errp) ||
        !dict_u64(dict, "read_set", 0, &reg->read_set, errp) ||
        !dict_u64(dict, "read_clear", 0, &reg->read_clear, errp) ||
        !dict_u64(dict, "w1c", 0, &reg->w1c, errp) ||
        !dict_u64(dict, "w1s", 0, &reg->w1s, errp) ||
        !parse_sequence(dict, reg, errp) ||
        !parse_triggers(dict, reg, errp)) {
        bsp_register_free(reg);
        return false;
    }
    if ((width != 1 && width != 2 && width != 4) ||
        reg->offset == UINT64_MAX || reg->offset + width > block->size) {
        error_setg(errp, "mcf-bsp-compat: register '%s' has invalid offset/width",
                   reg->name);
        bsp_register_free(reg);
        return false;
    }
    reg->width = width;
    if (find_register(block, reg->offset, reg->width)) {
        error_setg(errp, "mcf-bsp-compat: duplicate register at %s+0x%" PRIx64
                   " width %u", block->name, reg->offset, reg->width);
        bsp_register_free(reg);
        return false;
    }
    reg->value &= width_mask(reg->width);
    reg->writable_mask &= width_mask(reg->width);
    g_ptr_array_add(block->registers, reg);
    return true;
}

static McfBspBlock *new_block(McfBspCompatState *s, const char *name,
                              uint64_t base, uint64_t size)
{
    McfBspBlock *block = g_new0(McfBspBlock, 1);

    block->owner = s;
    block->name = g_strdup(name);
    block->base = base;
    block->size = size;
    block->registers = g_ptr_array_new_with_free_func(bsp_register_free);
    return block;
}

static bool parse_block(McfBspCompatState *s, QDict *dict, Error **errp)
{
    const char *name = qdict_get_try_str(dict, "name");
    uint64_t base, size;
    QList *registers;
    const QListEntry *entry;
    McfBspBlock *block;

    if (!name ||
        !dict_u64(dict, "base", UINT64_MAX, &base, errp) ||
        !dict_u64(dict, "size", 0, &size, errp)) {
        if (!name && errp && *errp == NULL) {
            error_setg(errp, "mcf-bsp-compat: block name is required");
        }
        return false;
    }
    if (!size || !target_block(name, base, size)) {
        error_setg(errp, "mcf-bsp-compat: block '%s' is not an unmodeled "
                   "MCF5208 range (base=0x%" PRIx64 ", size=0x%" PRIx64 ")",
                   name, base, size);
        return false;
    }
    for (guint i = 0; i < s->blocks->len; i++) {
        McfBspBlock *old = g_ptr_array_index(s->blocks, i);
        if (base < old->base + old->size && old->base < base + size) {
            error_setg(errp, "mcf-bsp-compat: overlapping profile blocks '%s' "
                       "and '%s'", old->name, name);
            return false;
        }
    }

    block = new_block(s, name, base, size);
    registers = qdict_get_qlist(dict, "registers");
    if (registers) {
        QLIST_FOREACH_ENTRY(registers, entry) {
            QDict *rd = qobject_to(QDict, qlist_entry_obj(entry));
            if (!rd || !parse_register(block, rd, errp)) {
                if (!rd && errp && *errp == NULL) {
                    error_setg(errp, "mcf-bsp-compat: register entry must be an object");
                }
                bsp_block_free(block);
                return false;
            }
        }
    }
    g_ptr_array_add(s->blocks, block);
    return true;
}

static bool load_profile(McfBspCompatState *s, Error **errp)
{
    g_autofree char *contents = NULL;
    gsize length = 0;
    g_autoptr(GError) gerr = NULL;
    QObject *root_obj;
    QDict *root;
    QList *blocks;
    const QListEntry *entry;
    Error *json_err = NULL;
    int64_t version;
    const char *target;

    if (!g_file_get_contents(s->profile, &contents, &length, &gerr)) {
        error_setg(errp, "mcf-bsp-compat: cannot read profile '%s': %s",
                   s->profile, gerr->message);
        return false;
    }
    root_obj = qobject_from_json(contents, &json_err);
    if (!root_obj) {
        error_propagate_prepend(errp, json_err,
                                "mcf-bsp-compat: invalid profile JSON: ");
        return false;
    }
    root = qobject_to(QDict, root_obj);
    if (!root) {
        error_setg(errp, "mcf-bsp-compat: profile root must be an object");
        qobject_unref(root_obj);
        return false;
    }
    version = qdict_get_try_int(root, "version", 0);
    target = qdict_get_try_str(root, "target");
    blocks = qdict_get_qlist(root, "blocks");
    if (version != 1 || !target || strcmp(target, "mcf5208") || !blocks) {
        error_setg(errp, "mcf-bsp-compat: profile requires version=1, "
                   "target='mcf5208', and a blocks array");
        qobject_unref(root_obj);
        return false;
    }
    QLIST_FOREACH_ENTRY(blocks, entry) {
        QDict *bd = qobject_to(QDict, qlist_entry_obj(entry));
        if (!bd || !parse_block(s, bd, errp)) {
            if (!bd && errp && *errp == NULL) {
                error_setg(errp, "mcf-bsp-compat: block entry must be an object");
            }
            qobject_unref(root_obj);
            return false;
        }
    }
    qobject_unref(root_obj);
    return true;
}

static void add_discovery_blocks(McfBspCompatState *s)
{
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(mcf5208_blocks); i++) {
        const MCFBlockDesc *d = &mcf5208_blocks[i];
        g_ptr_array_add(s->blocks, new_block(s, d->name, d->base, d->size));
    }
}

static void mcf_bsp_compat_realize(DeviceState *dev, Error **errp)
{
    McfBspCompatState *s = MCF_BSP_COMPAT(dev);
    guint i;

    s->blocks = g_ptr_array_new_with_free_func(bsp_block_free);
    if (!s->target || strcmp(s->target, "mcf5208")) {
        error_setg(errp, "mcf-bsp-compat: only target=mcf5208 is supported");
        return;
    }
    if (s->profile && s->profile[0]) {
        if (!load_profile(s, errp)) {
            return;
        }
    } else if (s->discover) {
        add_discovery_blocks(s);
    } else {
        error_setg(errp, "mcf-bsp-compat: profile is required unless "
                   "discover=on");
        return;
    }
    if (s->log_path && s->log_path[0]) {
        s->log_file = fopen(s->log_path, "w");
        if (!s->log_file) {
            error_setg_errno(errp, errno, "mcf-bsp-compat: cannot open log '%s'",
                             s->log_path);
            return;
        }
    }

    for (i = 0; i < s->blocks->len; i++) {
        McfBspBlock *block = g_ptr_array_index(s->blocks, i);
        memory_region_init_io(&block->region, OBJECT(dev), &bsp_ops, block,
                              block->name, block->size);
        memory_region_add_subregion_overlap(get_system_memory(), block->base,
                                            &block->region, 1);
        block->mapped = true;
    }
}

static void mcf_bsp_compat_unrealize(DeviceState *dev)
{
    McfBspCompatState *s = MCF_BSP_COMPAT(dev);

    if (s->log_file) {
        fclose(s->log_file);
        s->log_file = NULL;
    }
    g_clear_pointer(&s->blocks, g_ptr_array_unref);
}

static Property mcf_bsp_compat_properties[] = {
    DEFINE_PROP_STRING("target", McfBspCompatState, target),
    DEFINE_PROP_STRING("profile", McfBspCompatState, profile),
    DEFINE_PROP_STRING("log", McfBspCompatState, log_path),
    DEFINE_PROP_BOOL("discover", McfBspCompatState, discover, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void mcf_bsp_compat_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcf_bsp_compat_realize;
    dc->unrealize = mcf_bsp_compat_unrealize;
    dc->user_creatable = true;
    device_class_set_props(dc, mcf_bsp_compat_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo mcf_bsp_compat_info = {
    .name = TYPE_MCF_BSP_COMPAT,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(McfBspCompatState),
    .class_init = mcf_bsp_compat_class_init,
};

static void mcf_bsp_compat_register_types(void)
{
    type_register_static(&mcf_bsp_compat_info);
}

type_init(mcf_bsp_compat_register_types)
