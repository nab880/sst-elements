/*
 * raptor_gpio_blocks.h -- GENERATED; do not edit.
 *
 * Regenerate with:
 *   tools/gen_bsp_compat_blocks.py boards/raptor/board.json \
 *       --write-gpio-header qemu-overlay/hw/misc/raptor_gpio_blocks.h
 *
 * Source of truth: boards/raptor/board.json (regions carrying a
 * bsp_compat object with model "gpio"). These are the GPIO banks (GPIOB0-3) the mcf-gpio device maps: 16-bit registers, mask-in-high-byte value writes, read-modify-write direction, readback-correct.
 * They are deliberately excluded from the mcf-bsp-compat allowlist
 * in raptor_bsp_blocks.h so two devices never claim the same aperture.
 */

#ifndef QUETZ_RAPTOR_GPIO_BLOCKS_H
#define QUETZ_RAPTOR_GPIO_BLOCKS_H

#include <stdint.h>

#define RAPTOR_GPIO_TARGET "raptor"

typedef struct RaptorGpioBlockDesc {
    const char *name;
    uint64_t base;
    uint64_t size;
} RaptorGpioBlockDesc;

static const RaptorGpioBlockDesc raptor_gpio_blocks[] = {
    { "gpiob0", 0xfc084000, 0x4000 },
    { "gpiob1", 0xfc088000, 0x4000 },
    { "gpiob2", 0xfc08c000, 0x4000 },
    { "gpiob3", 0xfc090000, 0x4000 },
};

#endif /* QUETZ_RAPTOR_GPIO_BLOCKS_H */
