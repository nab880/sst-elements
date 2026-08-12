/*
 * raptor_dtimer_blocks.h -- GENERATED; do not edit.
 *
 * Regenerate with:
 *   tools/gen_bsp_compat_blocks.py boards/raptor/board.json \
 *       --write-dtimer-header qemu-overlay/hw/misc/raptor_dtimer_blocks.h
 *
 * Source of truth: boards/raptor/board.json (regions carrying a
 * bsp_compat object with model "dtimer"). These are the DMA-timer modules (DTIM0-3) the mcf-dtimer device maps and drives with a virtual-time counter.
 * They are deliberately excluded from the mcf-bsp-compat allowlist
 * in raptor_bsp_blocks.h so two devices never claim the same aperture.
 */

#ifndef QUETZ_RAPTOR_DTIMER_BLOCKS_H
#define QUETZ_RAPTOR_DTIMER_BLOCKS_H

#include <stdint.h>

#define RAPTOR_DTIMER_TARGET "raptor"

typedef struct RaptorDtimerBlockDesc {
    const char *name;
    uint64_t base;
    uint64_t size;
} RaptorDtimerBlockDesc;

static const RaptorDtimerBlockDesc raptor_dtimer_blocks[] = {
    { "dtim0", 0xfc070000, 0x4000 },
    { "dtim1", 0xfc074000, 0x4000 },
    { "dtim2", 0xfc078000, 0x4000 },
    { "dtim3", 0xfc07c000, 0x4000 },
};

#endif /* QUETZ_RAPTOR_DTIMER_BLOCKS_H */
