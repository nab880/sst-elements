/*
 * coldfire_xt_scale_stress.c — broader integration-level data sweep for
 * ScaleOffsetKernel than the existing accel_scale test (64 samples off a
 * fixed fixture): 512 synthetic samples (1024 bytes -- 16 DMA chunks of
 * kOpDmaChunk=64 bytes each direction, stressing the chunked-DMA loop far
 * more than the small existing test), with explicit extreme values forced
 * at the front (INT16_MIN, INT16_MAX, 0, -1) plus a full-range ramp, and an
 * aggressive scale/offset chosen to guarantee saturation at BOTH clamp
 * edges for a meaningful fraction of samples. The host-side kernel math is
 * already unit-tested (tests/unit/test_scale_offset.cc per the original
 * review); this is the integration path -- CPU byte-write -> sync-mmio
 * mailbox -> device DMA-read -> compute -> DMA-write -> CPU byte-read --
 * exercised at scale.
 *
 * The ColdFire deck and this fixture use the default BE window/kernel layout.
 *
 * SDL: sysmode/basic_quetz_gpu_compute_coldfire.py (QUETZ_KERNEL=
 * quetz.ScaleOffsetKernel).
 */

#include <stdint.h>

#include "coldfire_uart.h"
#include "coldfire_devices.h"
#include "coldfire_scale_ref.h"

#define N_SAMPLES  512u
#define SCALE      3
#define OFFSET     20000

#define WIN        0x71000000UL
#define IN_ADDR    (WIN + 0x0000UL)
#define OUT_ADDR   (WIN + 0x1000UL)

/* Front-loaded extremes, then a ramp spanning nearly the full s16 range. */
static int16_t sample(uint32_t i)
{
    switch (i) {
    case 0: return (int16_t)0x8000;   /* INT16_MIN */
    case 1: return (int16_t)0x7FFF;   /* INT16_MAX */
    case 2: return 0;
    case 3: return -1;
    default: return (int16_t)((int32_t)i * 137 - 30000);
    }
}

static int16_t expect(int16_t s)
{
    return cf_scale_offset_ref(s, SCALE, OFFSET);
}

void kernel_main(void)
{
    uart_init();
    uart_puts("ColdFire scale/offset stress: N=512, edge values (m68k)\n");

    /* Byte-write each sample individually (not packed u32 stores) so this
     * exercises the same byte-granular window path the BE alias probe
     * checks, at volume. The default ColdFire layout is [hi, lo]. */
    for (uint32_t i = 0; i < N_SAMPLES; i++) {
        uint16_t s = (uint16_t)sample(i);
        *(volatile uint8_t *)(IN_ADDR + 2 * i + 0) = (uint8_t)(s >> 8);
        *(volatile uint8_t *)(IN_ADDR + 2 * i + 1) = (uint8_t)(s & 0xFF);
    }

    mmio_write32(GPU_ARG0, (uint32_t)IN_ADDR);
    mmio_write32(GPU_ARG1, (uint32_t)OUT_ADDR);
    mmio_write32(GPU_ARG2, N_SAMPLES);
    mmio_write32(GPU_ARG3, (uint32_t)(uint16_t)SCALE
                         | ((uint32_t)(uint16_t)OFFSET << 16));
    mmio_write32(GPU_DOORBELL, 0);     /* blocking */

    uint32_t correct = 0, sat_hi = 0, sat_lo = 0;
    for (uint32_t i = 0; i < N_SAMPLES; i++) {
        uint8_t hi = *(volatile uint8_t *)(OUT_ADDR + 2 * i + 0);
        uint8_t lo = *(volatile uint8_t *)(OUT_ADDR + 2 * i + 1);
        int16_t got = (int16_t)((uint16_t)lo | ((uint16_t)hi << 8));
        int16_t want = expect(sample(i));
        if (got == want)
            correct++;
        if (want == 32767)  sat_hi++;
        if (want == -32768) sat_lo++;
    }

    uart_puts("scale stress: correct=");
    uart_put_u32_dec(correct);
    uart_putc('/');
    uart_put_u32_dec(N_SAMPLES);
    uart_puts(" sat_hi=");
    uart_put_u32_dec(sat_hi);
    uart_puts(" sat_lo=");
    uart_put_u32_dec(sat_lo);
    uart_putc('\n');

    int pass = (correct == N_SAMPLES) && (sat_hi > 0) && (sat_lo > 0);
    uart_puts(pass ? "SCALE STRESS PASS\n" : "SCALE STRESS FAIL\n");
    testdev_done(pass ? TESTDEV_PASS : TESTDEV_FAIL);
}
