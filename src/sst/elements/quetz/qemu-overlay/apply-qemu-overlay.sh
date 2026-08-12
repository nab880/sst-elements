#!/bin/sh
# Apply Quetz MMIO overlay into an extracted QEMU ${QEMU_VERSION} source tree.
# Fail loud on any error — we cannot silently degrade.
set -eu
QEMU_SRC="${1:-.}"
OVERLAY="$(cd "$(dirname "$0")" && pwd)"

[ -d "$QEMU_SRC/hw/misc" ] || { echo "ERROR: $QEMU_SRC does not look like QEMU"; exit 1; }

cp "$OVERLAY/hw/misc/sst_mmio_bridge.c" "$QEMU_SRC/hw/misc/sst_mmio_bridge.c"
cp "$OVERLAY/hw/misc/mcf_bsp_compat.c" "$QEMU_SRC/hw/misc/mcf_bsp_compat.c"
cp "$OVERLAY/hw/misc/raptor_bsp_blocks.h" "$QEMU_SRC/hw/misc/raptor_bsp_blocks.h"
cp "$OVERLAY/hw/misc/mcf_dtimer.c" "$QEMU_SRC/hw/misc/mcf_dtimer.c"
cp "$OVERLAY/hw/misc/raptor_dtimer_blocks.h" "$QEMU_SRC/hw/misc/raptor_dtimer_blocks.h"
cp "$OVERLAY/hw/misc/mcf_gpio.c" "$QEMU_SRC/hw/misc/mcf_gpio.c"
cp "$OVERLAY/hw/misc/raptor_gpio_blocks.h" "$QEMU_SRC/hw/misc/raptor_gpio_blocks.h"
cp "$OVERLAY/quetz_ipc_client.c"        "$QEMU_SRC/hw/misc/quetz_ipc_client.c"

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
if ! grep -q mcf_bsp_compat.c "$HW_MESON"; then
    cat >> "$HW_MESON" <<'EOF'
system_ss.add(files('mcf_bsp_compat.c'))
EOF
fi
if ! grep -q mcf_dtimer.c "$HW_MESON"; then
    cat >> "$HW_MESON" <<'EOF'
system_ss.add(files('mcf_dtimer.c'))
EOF
fi
if ! grep -q mcf_gpio.c "$HW_MESON"; then
    cat >> "$HW_MESON" <<'EOF'
system_ss.add(files('mcf_gpio.c'))
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
fi

# --- target/m68k/helper.c: accept RAMBAR (control reg 0xC04/0xC05) movec -----
# QEMU's ColdFire movec handler (cf_movec_to) aborts the guest on any control
# register it does not model:
#   qemu: fatal: Unimplemented control register write 0xc05 = ...
# The stock raptor-bsp crt0.c programs RAMBAR (0xC05) very early
#   (RAMBAR := __RAMBAR_START | (1<<9) | 1), so an unmodified BSP dies at boot.
# RAMBAR configures the local-SRAM base/backdoor aperture, which has no
# behavioral effect in QEMU's flat-SDRAM functional model. Store it in the
# existing (declared-but-unused) env->rambar0 field for fidelity/read-back
# rather than discarding it. Anchor-based + idempotent, like the edits above.
if [ -f "$QEMU_SRC/target/m68k/helper.c" ]; then
    QEMU_SRC="$QEMU_SRC" python3 - <<'PY'
import os
src = os.environ["QEMU_SRC"]
p = os.path.join(src, "target/m68k/helper.c")
s = open(p).read()
marker = "case M68K_CR_RAMBAR0:"
if marker not in s:
    # Insert RAMBAR handling into cf_movec_to, just before its default abort.
    default_line = "    /* TODO: Implement control registers.  */\n    default:\n"
    assert default_line in s, "anchor missing in target/m68k/helper.c (cf_movec_to)"
    ins = ("    case M68K_CR_RAMBAR0:\n"
           "    case M68K_CR_RAMBAR1:\n"
           "        /* Quetz overlay: RAMBAR configures the local-SRAM base and\n"
           "         * backdoor aperture, inert in the flat-SDRAM functional\n"
           "         * model. Store it (stock raptor-bsp crt0.c programs RAMBAR at\n"
           "         * boot) so it reads back, instead of aborting the guest. */\n"
           "        env->rambar0 = val;\n"
           "        break;\n")
    s = s.replace(default_line, ins + default_line, 1)
    open(p, "w").write(s)
print("cf_movec_to RAMBAR overlay applied")
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
