/*
 * coldfire_gpu_fft_offload.c — 256-pt FFT computed ON THE DEVICE (m68k).
 * QuetzGpuDevice kernel_type=fft: the guest fills the input, programs
 * REG_FFT_IN_ADDR/OUT_ADDR/N, rings the doorbell, verifies — no guest FFT math.
 * The FFT window is a sync-MMIO aperture, so guest stores carry values SST
 * serializes LE (the device's LE-float32 format) — no BE marshalling, and the
 * FPU-less core just compares raw IEEE bits. SDL: basic_quetz_gpu_compute_coldfire.py.
 */

#include <stdint.h>

#include "coldfire_uart.h"        /* uart_* + TESTDEV/testdev_done */

#define GPU_BASE          0x70000000UL
#define GPU_DOORBELL      (GPU_BASE + 0x00UL)   /* W: submit (blocking) */
#define GPU_STATUS        (GPU_BASE + 0x08UL)   /* R: busy(1)/idle(0) */
#define GPU_FFT_IN_ADDR   (GPU_BASE + 0x30UL)   /* W: input  cfloat[N] addr */
#define GPU_FFT_OUT_ADDR  (GPU_BASE + 0x38UL)   /* W: output cfloat[N] addr */
#define GPU_FFT_N         (GPU_BASE + 0x40UL)   /* W: N (power of two) */

#define FFT_N     256u
#define F32_ONE   0x3F800000u    /* 1.0f bit pattern */
#define F32_ZERO  0x00000000u

/* FFT buffers in the SST-backed window: interleaved {re, im} float32 pairs,
 * accessed as raw u32 bit patterns (2 words per point). */
#define FFT_WIN   0x71000000UL
static volatile uint32_t *const fft_in  = (volatile uint32_t *)(FFT_WIN + 0x0000);
static volatile uint32_t *const fft_out = (volatile uint32_t *)(FFT_WIN + 0x1000);

static inline void mmio_write32(uint32_t addr, uint32_t v)
{
    *(volatile uint32_t *)addr = v;
}

static inline uint32_t mmio_read32(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

void kernel_main(void)
{
    uart_init();
    uart_puts("ColdFire GPU FFT offload (device-computed, m68k)\n");

    /* impulse: x[0] = 1+0j, rest 0 — raw float32 bit patterns */
    for (uint32_t i = 0; i < FFT_N; i++) {
        fft_in[2u * i + 0u] = (i == 0) ? F32_ONE : F32_ZERO;
        fft_in[2u * i + 1u] = F32_ZERO;
    }

    /* program the device and fire — the DEVICE does the FFT */
    mmio_write32(GPU_FFT_IN_ADDR,  (uint32_t)FFT_WIN + 0x0000u);
    mmio_write32(GPU_FFT_OUT_ADDR, (uint32_t)FFT_WIN + 0x1000u);
    mmio_write32(GPU_FFT_N,        FFT_N);
    mmio_write32(GPU_DOORBELL,     0);   /* blocking: returns when result ready */

    /* impulse FFT -> 1+0j everywhere: compare raw bit patterns, no FPU needed */
    uint32_t correct = 0;
    for (uint32_t i = 0; i < FFT_N; i++) {
        if (fft_out[2u * i + 0u] == F32_ONE)  correct++;
        if (fft_out[2u * i + 1u] == F32_ZERO) correct++;
    }

    uart_puts("GPU FFT offload correct_words=");
    uart_put_u32_dec(correct);
    uart_putc('/');
    uart_put_u32_dec(2u * FFT_N);
    uart_putc('\n');

    testdev_done(correct == 2u * FFT_N ? TESTDEV_PASS : TESTDEV_FAIL);
}
