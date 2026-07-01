/*
 * coldfire_gpu_fft.c — 256-pt FFT on the SYNTHETIC GPU (m68k, balar-free).
 *
 * The ColdFire (32-bit big-endian, no FPU) counterpart of riscv_virt_gpu_fft.c.
 * The FFT math runs on the guest CPU in soft-float (libgcc __*sf3 helpers,
 * -msoft-float); the synthetic QuetzGpuDevice is used purely as a TIMING model,
 * with the same 11-kernel structure (1 H2D + 1 bitrev + 8 stages + 1 D2H) timed
 * by ringing the doorbell. Impulse input -> X[k]=1+0j, verified bit-exactly
 * (correct_words=512/512). No balar, no GPGPU-Sim, no wire marshalling — the FFT
 * data never leaves guest RAM, so the big-endian balar packing in
 * coldfire_balar.h is irrelevant here.
 *
 * SDL: sysmode/basic_quetz_gpu_coldfire.py (QuetzGpuDevice at 0x70000000, SDRAM
 * at 0x40000000, UART at 0xfc060000, TestFinisher sentinel at 0x80000000).
 */

#include <stdint.h>

#include "coldfire_uart.h"        /* uart_* + TESTDEV/testdev_done + sentinel */
#include "fft_synth_compute.h"    /* cfloat, fft_* ; pulls in fft_firmware_data_256.h */

/* Synthetic GPU device MMIO (same register map as the RISC-V synthetic GPU, but
 * 32-bit accesses from the 32-bit ColdFire core). Base at 0x70000000, where the
 * coldfire balar doorbell lives — here it is the synthetic latency device. */
#define GPU_BASE          0x70000000UL
#define GPU_DOORBELL      (GPU_BASE + 0x00UL)   /* W: submit */
#define GPU_STATUS        (GPU_BASE + 0x08UL)   /* R: busy(1)/idle(0) */
#define GPU_KERNEL_ID     (GPU_BASE + 0x10UL)   /* R: completed-ticket counter */
#define GPU_LATENCY_OVR   (GPU_BASE + 0x18UL)   /* W: next-kernel latency cycles */

static inline void mmio_write32(uint32_t addr, uint32_t v)
{
    *(volatile uint32_t *)addr = v;
}

static inline uint32_t mmio_read32(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

/* One timed "kernel": set the runtime, ring the doorbell, spin until IDLE. */
static uint32_t g_last_kernel_id;

static void launch_timed(uint32_t latency_cycles)
{
    mmio_write32(GPU_LATENCY_OVR, latency_cycles);
    mmio_write32(GPU_DOORBELL, 0);
    while (mmio_read32(GPU_STATUS))
        ;
    g_last_kernel_id = mmio_read32(GPU_KERNEL_ID);
}

#define FFT_KERNEL_CYCLES  5000u

static cfloat host_in[FFT_N];
static cfloat host_a[FFT_N];
static cfloat host_out[FFT_N];

void kernel_main(void)
{
    uart_init();
    uart_puts("ColdFire GPU FFT (synthetic, NXP mcf5208evb / m68k)\n");

    /* impulse: x[0] = 1+0j, rest 0 */
    for (uint32_t i = 0; i < FFT_N; i++) {
        host_in[i].re = (i == 0) ? FFT_ONE : FFT_ZERO;
        host_in[i].im = FFT_ZERO;
    }

    /* 11 timed kernels mirroring the balar FFT call structure. */
    launch_timed(FFT_KERNEL_CYCLES);                 /* [1] H2D (timing only) */

    fft_permute_bitrev(host_a, host_in, FFT_N, FFT_LOGN);
    launch_timed(FFT_KERNEL_CYCLES);                 /* [2] fft_bitrev */

    for (uint32_t s = 1; s <= FFT_LOGN; s++) {
        fft_stage(host_a, FFT_N, s, FFT_LOGN);
        launch_timed(FFT_KERNEL_CYCLES);             /* [3..10] fft_stage x8 */
    }

    for (uint32_t i = 0; i < FFT_N; i++)
        host_out[i] = host_a[i];
    launch_timed(FFT_KERNEL_CYCLES);                 /* [11] D2H (timing only) */

    uint32_t correct = fft_verify_impulse(host_out, FFT_N);
    uart_puts("GPU FFT (synthetic) correct_words=");
    uart_put_u32_dec(correct);
    uart_putc('/');
    uart_put_u32_dec(2u * FFT_N);
    uart_putc('\n');

    testdev_done(correct == 2u * FFT_N ? TESTDEV_PASS : TESTDEV_FAIL);
}
