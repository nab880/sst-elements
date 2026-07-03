/*
 * coldfire_accel_scale.c — sensor-batch processing ON THE DEVICE
 * (quetz.ScaleOffsetKernel), ColdFire m68k.
 *
 * The proof that the QuetzKernel API is not FFT-shaped: the same doorbell/
 * DMA machinery runs a saturating int16 scale/offset — the shape of a
 * sensor-path accelerator (calibration / normalization). The guest fills a
 * batch of s16 samples in the SST-backed window, programs the ARG registers,
 * rings the blocking doorbell, and verifies the transformed batch against a
 * CPU-computed expectation (bit-exact, including both saturation edges).
 *
 * Value convention (same trick as coldfire_gpu_fft_offload.c): the bridge
 * mailbox carries *values* and SST serializes them little-endian into window
 * RAM, so packing two samples per u32 store — s0 in bits[15:0], s1 in
 * bits[31:16] — yields the kernel's LE s16 stream with no guest byte-swapping,
 * on a big-endian core.
 *
 * SDL: sysmode/basic_quetz_gpu_compute_coldfire.py with
 * QUETZ_KERNEL=quetz.ScaleOffsetKernel (GPU MMIO at 0x70000000, window at
 * 0x71000000, UART at 0xfc060000, TestFinisher at 0x80000000).
 */

#include <stdint.h>

#include "coldfire_uart.h"        /* uart_* + TESTDEV/testdev_done */

#define GPU_BASE          0x70000000UL
#define GPU_DOORBELL      (GPU_BASE + 0x00UL)   /* W: submit (blocking) */
#define GPU_STATUS        (GPU_BASE + 0x08UL)   /* R: busy(1)/idle(0) */
#define GPU_ARG0          (GPU_BASE + 0x30UL)   /* W: input  s16[N] addr */
#define GPU_ARG1          (GPU_BASE + 0x38UL)   /* W: output s16[N] addr */
#define GPU_ARG2          (GPU_BASE + 0x40UL)   /* W: sample count N */
#define GPU_ARG3          (GPU_BASE + 0x48UL)   /* W: scale | offset<<16 */

#define N_SAMPLES  64u
#define SCALE      3
#define OFFSET     500

#define WIN        0x71000000UL
static volatile uint32_t *const buf_in  = (volatile uint32_t *)(WIN + 0x0000);
static volatile uint32_t *const buf_out = (volatile uint32_t *)(WIN + 0x1000);

static inline void mmio_write32(uint32_t addr, uint32_t v)
{
    *(volatile uint32_t *)addr = v;
}

static inline uint32_t mmio_read32(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

/* Sample generator: spans both saturation edges at SCALE/OFFSET. */
static int16_t sample(uint32_t i)
{
    return (int16_t)((int32_t)i * 900 - 28000);
}

/* Mirror of quetz_scale_offset.h:quetz_scale_offset_sat16(). */
static int16_t expect(int16_t s)
{
    int32_t v = (int32_t)s * SCALE + OFFSET;
    if (v > 32767)  return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
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
    while (mmio_read32(GPU_STATUS))      /* (belt-and-suspenders) */
        ;

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
