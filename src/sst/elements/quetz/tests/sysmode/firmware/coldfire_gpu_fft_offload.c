/*
 * coldfire_gpu_fft_offload.c — 256-pt FFT computed ON THE DEVICE (m68k).
 *
 * The ColdFire counterpart of riscv_virt_gpu_fft_offload.c: QuetzGpuDevice runs
 * in kernel_type=fft mode, so the guest only fills the input buffer, programs
 * REG_FFT_IN_ADDR/OUT_ADDR/N, rings the doorbell, and verifies the result. No
 * FFT math on the guest at all — and unlike the CPU-compute coldfire_gpu_fft.c,
 * no Q16.16 fixed point either, because the device computes in host float.
 *
 * Endianness: none of the big-endian marshalling from coldfire_balar.h is
 * needed. The FFT window is a sync-MMIO aperture (sst-mmio-bridge), so guest
 * accesses carry VALUES, not raw guest-memory bytes: QEMU decodes the store to
 * a value and SST serializes that value little-endian into the window RAM —
 * the device's canonical LE-float32 wire format. The FPU-less ColdFire never
 * needs float ops either: it stores/compares the raw IEEE bit patterns
 * (0x3F800000 == 1.0f) as plain uint32_t.
 *
 * SDL: sysmode/basic_quetz_gpu_compute_coldfire.py (GPU MMIO at 0x70000000,
 * FFT window at 0x71000000, SDRAM at 0x40000000, UART at 0xfc060000,
 * TestFinisher sentinel at 0x80000000).
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
    while (mmio_read32(GPU_STATUS))      /* (belt-and-suspenders) */
        ;

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
