#!/bin/sh
# Apply Quetz MMIO overlay into an extracted QEMU ${QEMU_VERSION} source tree.
# Fail loud on any error — we cannot silently degrade.
set -eu
QEMU_SRC="${1:-.}"
OVERLAY="$(cd "$(dirname "$0")" && pwd)"

[ -d "$QEMU_SRC/hw/misc" ] || { echo "ERROR: $QEMU_SRC does not look like QEMU"; exit 1; }

cp "$OVERLAY/hw/misc/sst_mmio_bridge.c" "$QEMU_SRC/hw/misc/sst_mmio_bridge.c"
cp "$OVERLAY/hw/misc/mcf_bsp_compat.c" "$QEMU_SRC/hw/misc/mcf_bsp_compat.c"
cp "$OVERLAY/quetz_ipc_client.c"        "$QEMU_SRC/hw/misc/quetz_ipc_client.c"

mkdir -p "$QEMU_SRC/hw/m68k"
cp "$OVERLAY/hw/m68k/raptor.c" "$QEMU_SRC/hw/m68k/raptor.c"

mkdir -p "$QEMU_SRC/include/quetz"
cp "$OVERLAY/include/quetz/quetz_ipc_client.h" "$QEMU_SRC/include/quetz/"
cp "$OVERLAY/include/quetz/quetz_ipc_types.h"  "$QEMU_SRC/include/quetz/"

# Append softmmu source registrations to hw/misc/meson.build.
HW_MESON="$QEMU_SRC/hw/misc/meson.build"
if ! grep -q sst_mmio_bridge.c "$HW_MESON"; then
    cat >> "$HW_MESON" <<'EOF'

# Quetz MMIO bridge (added by sst overlay)
system_ss.add(files('sst_mmio_bridge.c'))
system_ss.add(files('quetz_ipc_client.c'))
EOF
fi

M68K_MESON="$QEMU_SRC/hw/m68k/meson.build"
if ! grep -q "files('raptor.c')" "$M68K_MESON"; then
    cat >> "$M68K_MESON" <<'EOF'

# Quetz Raptor Core2 functional-profile machine
m68k_ss.add(files('raptor.c'))
EOF
fi

# Accept and retain the two ColdFire RAMBAR registers used by the production
# CRT.  QEMU's cfv4e model otherwise aborts on these architected MOVEC writes.
QEMU_SRC="$QEMU_SRC" python3 - <<'PY'
import os

src = os.environ["QEMU_SRC"]

cpu_h = os.path.join(src, "target/m68k/cpu.h")
text = open(cpu_h).read()
if "uint32_t rambar1;" not in text:
    anchor = "    uint32_t rambar0;\n"
    assert anchor in text, "RAMBAR state anchor missing in target/m68k/cpu.h"
    text = text.replace(anchor, anchor + "    uint32_t rambar1;\n", 1)
    open(cpu_h, "w").write(text)

helper = os.path.join(src, "target/m68k/helper.c")
text = open(helper).read()
marker = "case M68K_CR_RAMBAR1:"
if marker not in text:
    anchor = "    case M68K_CR_VBR:\n        env->vbr = val;\n        break;\n"
    assert anchor in text, "ColdFire MOVEC anchor missing in target/m68k/helper.c"
    insert = (
        "    case M68K_CR_RAMBAR0:\n"
        "        env->rambar0 = val;\n"
        "        break;\n"
        "    case M68K_CR_RAMBAR1:\n"
        "        env->rambar1 = val;\n"
        "        break;\n"
    )
    text = text.replace(anchor, anchor + insert, 1)
    open(helper, "w").write(text)

cpu_c = os.path.join(src, "target/m68k/cpu.c")
text = open(cpu_c).read()
if "VMSTATE_UINT32_V(env.rambar1" not in text:
    start = text.index("const VMStateDescription vmstate_cf_spregs")
    end = text.index("};", start)
    block = text[start:end]
    block = block.replace(".version_id = 1", ".version_id = 2", 1)
    anchor = "        VMSTATE_UINT32(env.rambar0, M68kCPU),\n"
    assert anchor in block, "RAMBAR migration anchor missing in target/m68k/cpu.c"
    block = block.replace(
        anchor,
        anchor + "        VMSTATE_UINT32_V(env.rambar1, M68kCPU, 2),\n",
        1,
    )
    text = text[:start] + block + text[end:]
    open(cpu_c, "w").write(text)

print("Raptor RAMBAR overlay applied")
PY
if ! grep -q mcf_bsp_compat.c "$HW_MESON"; then
    cat >> "$HW_MESON" <<'EOF'
system_ss.add(files('mcf_bsp_compat.c'))
EOF
fi

# Mark device user-creatable through default Kconfig (sst-mmio-bridge is built
# unconditionally; no Kconfig symbol needed).

# --- hw/m68k/mcf_intc.c: expose the 64 interrupt inputs as qdev GPIOs -------
# The mcf5208evb machine wires its devices through qemu_allocate_irqs() and
# frees the array, so a foreign device (sst-mmio-bridge IRQ injection) has no
# path to the controller inputs. Registering them as qdev GPIO inputs makes
# them addressable via qdev_get_gpio_in(dev, line); mcf_intc_set_irq already
# has the qemu_irq_handler signature and casts its opaque from the device
# pointer, which is what qdev GPIOs pass. Anchor-based + idempotent, like the
# linux-user edits below.
if [ -f "$QEMU_SRC/hw/m68k/mcf_intc.c" ]; then
    QEMU_SRC="$QEMU_SRC" python3 - <<'PY'
import os
src = os.environ["QEMU_SRC"]
p = os.path.join(src, "hw/m68k/mcf_intc.c")
s = open(p).read()
if "qdev_init_gpio_in" not in s:
    anchor = ('    memory_region_init_io(&s->iomem, obj, &mcf_intc_ops, s,'
              ' "mcf", 0x100);\n')
    assert anchor in s, "anchor missing in hw/m68k/mcf_intc.c"
    ins = ("    /* Quetz overlay: expose the 64 interrupt inputs as qdev GPIOs\n"
           "     * so sst-mmio-bridge can inject SST-device IRQs by line. */\n"
           "    qdev_init_gpio_in(DEVICE(obj), mcf_intc_set_irq, 64);\n")
    s = s.replace(anchor, anchor + ins, 1)
    open(p, "w").write(s)
print("mcf_intc qdev GPIO overlay applied")
PY

    # Raptor's BSP programs each ICR as IL[5:3]|IP[2:0].  Keep stock
    # mcf5208evb's direct numeric-level convention as the default and select
    # IL/IP decoding only for the dedicated Raptor machine.
    QEMU_SRC="$QEMU_SRC" python3 - <<'PY'
import os
src = os.environ["QEMU_SRC"]
p = os.path.join(src, "hw/m68k/mcf_intc.c")
s = open(p).read()
if "il_ip_priority" not in s:
    s = s.replace(
        "    M68kCPU *cpu;\n    int active_vector;\n",
        "    M68kCPU *cpu;\n    bool il_ip_priority;\n    int active_vector;\n",
        1,
    )
    s = s.replace(
        "    int best_level;\n",
        "    int best_level;\n    int best_rank;\n",
        1,
    )
    s = s.replace(
        "    best_level = 0;\n    best = 64;\n",
        "    best_level = 0;\n    best_rank = -1;\n    best = 64;\n",
        1,
    )
    old = (
        "            if ((active & 1) != 0 && s->icr[i] >= best_level) {\n"
        "                best_level = s->icr[i];\n"
        "                best = i;\n"
        "            }\n"
    )
    new = (
        "            int rank = s->il_ip_priority ? (s->icr[i] & 0x3f)\n"
        "                                             : s->icr[i];\n"
        "            int level = s->il_ip_priority ? ((rank >> 3) & 7)\n"
        "                                              : rank;\n"
        "            if ((active & 1) != 0 && rank >= best_rank) {\n"
        "                best_rank = rank;\n"
        "                best_level = level;\n"
        "                best = i;\n"
        "            }\n"
    )
    assert old in s, "priority anchor missing in hw/m68k/mcf_intc.c"
    s = s.replace(old, new, 1)

    prop_anchor = (
        '    DEFINE_PROP_LINK("m68k-cpu", mcf_intc_state, cpu,\n'
        '                     TYPE_M68K_CPU, M68kCPU *),\n'
    )
    assert prop_anchor in s, "property anchor missing in hw/m68k/mcf_intc.c"
    s = s.replace(
        prop_anchor,
        prop_anchor
        + '    DEFINE_PROP_BOOL("il-ip-priority", mcf_intc_state,\n'
          '                     il_ip_priority, false),\n',
        1,
    )

    signature = (
        "qemu_irq *mcf_intc_init(MemoryRegion *sysmem,\n"
        "                        hwaddr base,\n"
        "                        M68kCPU *cpu)\n"
    )
    replacement = (
        "qemu_irq *mcf_intc_init_ext(MemoryRegion *sysmem,\n"
        "                            hwaddr base,\n"
        "                            M68kCPU *cpu,\n"
        "                            bool il_ip_priority)\n"
    )
    assert signature in s, "init anchor missing in hw/m68k/mcf_intc.c"
    s = s.replace(signature, replacement, 1)
    link_anchor = (
        '    object_property_set_link(OBJECT(dev), "m68k-cpu",\n'
        '                             OBJECT(cpu), &error_abort);\n'
    )
    assert link_anchor in s, "link anchor missing in hw/m68k/mcf_intc.c"
    s = s.replace(
        link_anchor,
        link_anchor
        + '    object_property_set_bool(OBJECT(dev), "il-ip-priority",\n'
          '                             il_ip_priority, &error_abort);\n',
        1,
    )
    s += (
        "\nqemu_irq *mcf_intc_init(MemoryRegion *sysmem, hwaddr base,\n"
        "                        M68kCPU *cpu)\n"
        "{\n"
        "    return mcf_intc_init_ext(sysmem, base, cpu, false);\n"
        "}\n"
    )
    open(p, "w").write(s)

header = os.path.join(src, "include/hw/m68k/mcf.h")
h = open(header).read()
if "mcf_intc_init_ext" not in h:
    anchor = (
        "qemu_irq *mcf_intc_init(struct MemoryRegion *sysmem,\n"
        "                        hwaddr base,\n"
        "                        M68kCPU *cpu);\n"
    )
    assert anchor in h, "INTC header anchor missing in include/hw/m68k/mcf.h"
    h = h.replace(
        anchor,
        anchor
        + "qemu_irq *mcf_intc_init_ext(struct MemoryRegion *sysmem,\n"
          "                            hwaddr base,\n"
          "                            M68kCPU *cpu,\n"
          "                            bool il_ip_priority);\n",
        1,
    )
    open(header, "w").write(h)
print("mcf_intc scoped functional priority overlay applied")
PY
fi

# --- linux-user (P6): SIGSEGV-trap synchronous MMIO --------------------------
# System mode traps the doorbell with the sst-mmio-bridge device; user mode has
# no device map, so qemu-<arch> reserves the aperture PROT_NONE and routes the
# resulting SIGSEGV to the same sync mailbox. Edits are anchor-based + idempotent
# so they tolerate QEMU point releases (developed against 9.2.1).
if [ -d "$QEMU_SRC/linux-user" ]; then
    cp "$OVERLAY/linux-user/sst_mmio.c" "$QEMU_SRC/linux-user/sst_mmio.c"
    cp "$OVERLAY/linux-user/sst_mmio.h" "$QEMU_SRC/linux-user/sst_mmio.h"

    QEMU_SRC="$QEMU_SRC" python3 - <<'PY'
import os
src = os.environ["QEMU_SRC"]
q = chr(39)

def patch(path, edits):
    p = os.path.join(src, path)
    s = open(p).read()
    for marker, anchor, ins, after in edits:
        if marker in s:
            continue
        assert anchor in s, "anchor missing in %s: %r" % (path, anchor)
        s = s.replace(anchor, (anchor + ins) if after else (ins + anchor), 1)
    open(p, "w").write(s)

# linux-user/meson.build: compile sst_mmio.c + ipc client into every target
# (linux_user_ss is built per-target; sst_mmio.c is internally TARGET_*-guarded).
patch("linux-user/meson.build", [(
    "sst_mmio.c",
    "linux_user_ss.add(rt)\n",
    "\n# Quetz user-mode synchronous MMIO (P6)\n"
    "linux_user_ss.add(files(" + q + "sst_mmio.c" + q + "))\n"
    "linux_user_ss.add(files(" + q + "../hw/misc/quetz_ipc_client.c" + q + "))\n",
    True)])

# linux-user/main.c: include, arg handler, arg_table entry, aperture reservation.
patch("linux-user/main.c", [
    ("sst_mmio.h", '#include "qemu.h"\n', '#include "sst_mmio.h"\n', True),
    ("handle_arg_sst_mmio_range",
     "static const struct qemu_argument arg_table[] = {\n",
     "static void handle_arg_sst_mmio_range(const char *arg)\n"
     "{\n    sst_mmio_register_range(arg);\n}\n\n", False),
    ("QEMU_SST_MMIO_RANGE",
     "    {NULL, NULL, false, NULL, NULL, NULL}\n};",
     '    {"sst-mmio-range", "QEMU_SST_MMIO_RANGE", true, handle_arg_sst_mmio_range,\n'
     '     "spec",       "Quetz sync MMIO range shmname=,base=,size= (repeatable)"},\n',
     False),
    ("sst_mmio_apply_reservation", "    cpu_loop(env);\n",
     "    sst_mmio_apply_reservation();\n", False),
])

# linux-user/signal.c: include + the host_sigsegv_handler hook.
patch("linux-user/signal.c", [
    ("sst_mmio.h", '#include "host-signal.h"\n', '#include "sst_mmio.h"\n', True),
    ("sst_mmio_handle_fault",
     "    MMUAccessType access_type = adjust_signal_pc(&pc, is_write);\n"
     "    bool maperr;\n",
     "\n    /* Quetz P6: route reserved-aperture faults to the sync mailbox.\n"
     "     * On a match this does not return (cpu_loop_exit). */\n"
     "    sst_mmio_handle_fault(cpu, guest_addr, pc);\n", True),
])
print("linux-user overlay applied")
PY
fi

echo "Quetz QEMU overlay applied under $QEMU_SRC"
