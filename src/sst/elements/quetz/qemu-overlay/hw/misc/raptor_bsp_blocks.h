/*
 * raptor_bsp_blocks.h -- GENERATED; do not edit.
 *
 * Regenerate with:
 *   tools/gen_bsp_compat_blocks.py boards/raptor/board.json \
 *       --write-header qemu-overlay/hw/misc/raptor_bsp_blocks.h
 *
 * Source of truth: boards/raptor/board.json (regions carrying a
 * bsp_compat object with model "compat" or none). This is the
 * sparse-block allowlist the mcf-bsp-compat device may overlay on
 * otherwise-unmodeled IPS space. Blocks owned by a dedicated device
 * model (bsp_compat.model "dtimer" or "gpio") are emitted separately
 * and excluded here so two devices never claim the same aperture.
 */

#ifndef QUETZ_RAPTOR_BSP_BLOCKS_H
#define QUETZ_RAPTOR_BSP_BLOCKS_H

#include <stdint.h>

#define RAPTOR_BSP_COMPAT_TARGET "raptor"

typedef struct RaptorBspBlockDesc {
    const char *name;
    uint64_t base;
    uint64_t size;
} RaptorBspBlockDesc;

static const RaptorBspBlockDesc raptor_bsp_blocks[] = {
    { "fbcs", 0xfc008000, 0x4000 },
    { "scm2", 0xfc040000, 0x4000 },
};

#endif /* QUETZ_RAPTOR_BSP_BLOCKS_H */
