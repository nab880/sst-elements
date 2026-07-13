/*
 * coldfire_xt_be_alias.c — completeness probe for the SST-window endianness
 * contract (review-coldfire-stack-correctness.md finding #2 /
 * window_big_endian + data_big_endian). Same firmware binary is run under
 * three SDL configurations (see expanded_coldfire_tests.py):
 *
 *   1. explicit legacy LE (window_big_endian=0): documents the pre-existing
 *      sub-word aliasing -- byte-write hi-then-lo (the natural m68k /
 *      big-endian convention), word-read comes back BYTE-SWAPPED.
 *   2. ColdFire default (window_big_endian=1, kernel data_big_endian=1):
 *      byte-write
 *      hi-then-lo, word-read comes back CORRECT, and the ScaleOffsetKernel
 *      round-trip (identity transform) also comes back CORRECT.
 *   3. mismatched (window_big_endian=1, kernel data_big_endian=0): the CPU
 *      stores window bytes MSB-first but the kernel still interprets them
 *      LE. The IDENTITY round-trip is byte-order-blind (misread bytes are
 *      miswritten straight back), so the footgun shows up in the second,
 *      value-changing probe (offset +0xCC): the kernel adds the offset to
 *      the byte-swapped sample and the result comes back scrambled.
 *
 * This firmware only REPORTS values (like bsp_torture.c); it does not judge
 * pass/fail itself since the "correct" outcome differs per configuration —
 * the testsuite asserts on the reported hex.
 *
 * SDL: sysmode/basic_quetz_gpu_compute_coldfire.py (QUETZ_KERNEL=
 * quetz.ScaleOffsetKernel, scale=1 offset=0 so output==input exactly).
 */

#include <stdint.h>

#include "coldfire_uart.h"
#include "coldfire_devices.h"

#define WIN               0x71000000UL
/* Raw byte/word aliasing probe -- no kernel involvement. */
#define RAW_ADDR          (WIN + 0x0000UL)
/* Kernel round-trip probe -- distinct region so the two don't collide. */
#define KIN_ADDR          (WIN + 0x0100UL)
#define KOUT_ADDR         (WIN + 0x1100UL)
/* Second kernel probe (scale=1, offset=+0xCC): an identity transform is
 * byte-order-blind (LE-misread then LE-miswritten bytes land back exactly
 * where they started), so only a value-CHANGING transform can expose the
 * mismatched-flags footgun. */
#define K2IN_ADDR         (WIN + 0x2100UL)
#define K2OUT_ADDR        (WIN + 0x3100UL)
#define K2_OFFSET         0xCC

void kernel_main(void)
{
    uart_init();
    uart_puts("ColdFire BE window alias probe (m68k)\n");

    /* --- Raw byte-write-then-word-read (no device involved) --------------
     * Write the BE-natural byte order for 0x1234 (hi byte at the lower
     * address -- how any m68k memcpy/byte-packing driver would lay it out),
     * then read the same address as one 16-bit word. */
    *(volatile uint8_t *)(RAW_ADDR + 0) = 0x12;
    *(volatile uint8_t *)(RAW_ADDR + 1) = 0x34;
    uint16_t word_read = *(volatile uint16_t *)(RAW_ADDR + 0);

    uart_puts("raw: wrote hi=0x12 lo=0x34, word_read=");
    uart_put_u32_hex(word_read);      /* prints its own "0x" + 8 hex digits */
    uart_putc('\n');

    /* --- Kernel round-trip (ScaleOffsetKernel, scale=1 offset=0) ----------
     * One s16 sample = 0x1234, written the same BE-natural way; the device
     * DMA-reads it, kernel copies it through unchanged, DMA-writes it back;
     * read the result back the same way. */
    *(volatile uint8_t *)(KIN_ADDR + 0) = 0x12;
    *(volatile uint8_t *)(KIN_ADDR + 1) = 0x34;

    mmio_write32(GPU_ARG0, (uint32_t)KIN_ADDR);
    mmio_write32(GPU_ARG1, (uint32_t)KOUT_ADDR);
    mmio_write32(GPU_ARG2, 1);                    /* N = 1 sample */
    mmio_write32(GPU_ARG3, (uint32_t)(uint16_t)1  /* scale=1 */
                         | ((uint32_t)(uint16_t)0 << 16));  /* offset=0 */
    mmio_write32(GPU_DOORBELL, 0);                /* blocking */

    uint16_t kernel_roundtrip = *(volatile uint16_t *)(KOUT_ADDR + 0);

    uart_puts("kernel roundtrip (identity scale=1/offset=0): ");
    uart_put_u32_hex(kernel_roundtrip);
    uart_putc('\n');

    /* --- Kernel round-trip #2 (scale=1, offset=+0xCC) ---------------------
     * The value changes, so the kernel's byte-order view is observable:
     * a kernel that misreads the sample byte-swapped adds the offset to the
     * WRONG value and the result no longer swaps back into place. */
    *(volatile uint8_t *)(K2IN_ADDR + 0) = 0x12;
    *(volatile uint8_t *)(K2IN_ADDR + 1) = 0x34;

    mmio_write32(GPU_ARG0, (uint32_t)K2IN_ADDR);
    mmio_write32(GPU_ARG1, (uint32_t)K2OUT_ADDR);
    mmio_write32(GPU_ARG2, 1);                    /* N = 1 sample */
    mmio_write32(GPU_ARG3, (uint32_t)(uint16_t)1  /* scale=1 */
                         | ((uint32_t)(uint16_t)K2_OFFSET << 16));
    mmio_write32(GPU_DOORBELL, 0);                /* blocking */

    uint16_t kernel_offset_rt = *(volatile uint16_t *)(K2OUT_ADDR + 0);

    uart_puts("kernel offset roundtrip (scale=1/offset=0xCC): ");
    uart_put_u32_hex(kernel_offset_rt);
    uart_putc('\n');

    /* Exploratory report, like bsp_torture -- the transcript is the result. */
    uart_puts("BE ALIAS PROBE DONE\n");
    testdev_done(TESTDEV_PASS);
}
