/*
 * coldfire_accel_scale.c — device-computed saturating int16 scale/offset
 * (quetz.ScaleOffsetKernel), ColdFire m68k. Samples are packed two per u32 store
 * so SST's LE value-serialization yields the kernel's LE s16 stream on the BE core.
 * SDL: sysmode/basic_quetz_gpu_compute_coldfire.py.
 */

#include <stdint.h>

#include "coldfire_uart.h"        /* uart_* + TESTDEV/testdev_done */
#include "coldfire_devices.h"
#include "coldfire_scale_ref.h"

#define N_SAMPLES  64u
#define SCALE      3
#define OFFSET     500

#define WIN        0x71000000UL
static volatile uint32_t *const buf_in  = (volatile uint32_t *)(WIN + 0x0000);
static volatile uint32_t *const buf_out = (volatile uint32_t *)(WIN + 0x1000);

/* Sample generator: spans both saturation edges at SCALE/OFFSET. */
static int16_t sample(uint32_t i)
{
    return (int16_t)((int32_t)i * 900 - 28000);
}

static int16_t expect(int16_t s)
{
    return cf_scale_offset_ref(s, SCALE, OFFSET);
}

void kernel_main(void)
{
    uart_init();
    uart_puts("ColdFire accel scale/offset (device-computed, m68k)\n");

    /* fill the input batch, two s16 per u32 (LE stream via value semantics) */
    for (uint32_t i = 0; i < N_SAMPLES; i += 2) {
        uint32_t w = (uint32_t)(uint16_t)sample(i)
                   | ((uint32_t)(uint16_t)sample(i + 1) << 16);
        buf_in[i / 2] = w;
    }

    /* program the device and fire — the DEVICE transforms the batch */
    mmio_write32(GPU_ARG0, (uint32_t)WIN + 0x0000u);
    mmio_write32(GPU_ARG1, (uint32_t)WIN + 0x1000u);
    mmio_write32(GPU_ARG2, N_SAMPLES);
    mmio_write32(GPU_ARG3, (uint32_t)(uint16_t)SCALE
                         | ((uint32_t)(uint16_t)OFFSET << 16));
    mmio_write32(GPU_DOORBELL, 0);       /* blocking: returns when done */

    /* verify the transformed batch, word at a time */
    uint32_t correct = 0;
    for (uint32_t i = 0; i < N_SAMPLES; i += 2) {
        uint32_t want = (uint32_t)(uint16_t)expect(sample(i))
                      | ((uint32_t)(uint16_t)expect(sample(i + 1)) << 16);
        if (buf_out[i / 2] == want)
            correct += 2;
    }

    uart_puts("accel scale correct_samples=");
    uart_put_u32_dec(correct);
    uart_putc('/');
    uart_put_u32_dec(N_SAMPLES);
    uart_putc('\n');

    testdev_done(correct == N_SAMPLES ? TESTDEV_PASS : TESTDEV_FAIL);
}
