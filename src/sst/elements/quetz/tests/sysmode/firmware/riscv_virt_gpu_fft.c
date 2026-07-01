/*
 * riscv_virt_gpu_fft.c — 256-point FFT on the SYNTHETIC GPU (no balar/GPGPU-Sim).
 *
 * The FFT math runs on the guest CPU (rv64gc hardware float); the synthetic
 * QuetzGpuDevice is used purely as a TIMING model: each "kernel" is timed by
 * ringing the device doorbell (LATENCY_OVERRIDE -> DOORBELL -> spin on STATUS).
 * To make this a true timing stand-in for the balar FFT run, we issue the SAME
 * number of doorbells as the balar call structure: 1 H2D + 1 bitrev + 8 stages +
 * 1 D2H = 11 kernels (the H2D/D2H doorbells are pure timing; the data never
 * leaves guest RAM).
 *
 * Impulse input (x[0]=1) -> X[k]=1+0j for all k, verified bit-exactly
 * (correct_words=512/512). No GPGPUSIM_ROOT, no .cfg, no fatbin.
 *
 * SDL: sysmode/basic_quetz_gpu.py (QuetzGpuDevice at 0x80100000, DRAM filtered).
 */

#include <stdint.h>

#include "fft_synth_compute.h"   /* cfloat, fft_* ; pulls in fft_firmware_data_256.h */

/* --- QEMU virt UART + SiFive test finisher (same map as the other rv firmwares) */
#define UART0_BASE   0x10000000UL
#define UART_THR     (*(volatile unsigned char*)(UART0_BASE + 0x00))
#define UART_LSR     (*(volatile unsigned char*)(UART0_BASE + 0x05))
#define LSR_THRE     (1u << 5)

#define TESTDEV      (*(volatile unsigned int*)0x100000UL)
#define TESTDEV_PASS 0x5555u
#define TESTDEV_FAIL 0x3333u

/* --- Synthetic GPU device MMIO (identical map to riscv_virt_gpu_kernel.c) ------ */
#define GPU_BASE         0x80100000UL
#define GPU_DOORBELL     (*(volatile unsigned long*)(GPU_BASE + 0x00))
#define GPU_STATUS       (*(volatile unsigned long*)(GPU_BASE + 0x08))
#define GPU_KERNEL_ID    (*(volatile unsigned long*)(GPU_BASE + 0x10))
#define GPU_LATENCY_OVR  (*(volatile unsigned long*)(GPU_BASE + 0x18))

static void uart_putc(char c)
{
    while (!(UART_LSR & LSR_THRE))
        ;
    UART_THR = (unsigned char)c;
}

static void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

static void uart_put_u32_dec(uint32_t v)
{
    char buf[12];
    unsigned i = 0;
    if (v == 0) {
        uart_putc('0');
        return;
    }
    while (v && i < sizeof(buf)) {
        buf[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (i)
        uart_putc(buf[--i]);
}

/* Ring the synthetic doorbell for one timed "kernel": set its runtime, submit,
 * and spin until the device leaves BUSY. last_kernel_id tracks the monotonic
 * completed-ticket counter (sanity). */
static volatile unsigned long last_kernel_id;

static void launch_timed(unsigned long latency_cycles)
{
    GPU_LATENCY_OVR = latency_cycles;
    GPU_DOORBELL = 0;
    while (GPU_STATUS)
        ;
    last_kernel_id = GPU_KERNEL_ID;
}

/* Default per-kernel latency; the SDL passes kernel_latency (QUETZ_GPU_LATENCY,
 * default 5000) but LATENCY_OVERRIDE wins per-launch, so pin it here for a
 * deterministic busy_cycles total (11 * FFT_KERNEL_CYCLES). */
#define FFT_KERNEL_CYCLES  5000UL

static cfloat host_in[FFT_N];
static cfloat host_a[FFT_N];   /* bit-reversed permutation */
static cfloat host_out[FFT_N]; /* final spectrum */

void kernel_main(void)
{
    /* impulse: x[0] = 1+0j, rest 0 */
    for (uint32_t i = 0; i < FFT_N; i++) {
        host_in[i].re = (i == 0) ? 1.0f : 0.0f;
        host_in[i].im = 0.0f;
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

    TESTDEV = (correct == 2u * FFT_N) ? TESTDEV_PASS : TESTDEV_FAIL;
    while (1)
        __asm__ volatile ("wfi");
}

/* QEMU virt `-bios none -kernel` jumps to the fixed load address 0x80000000
 * (start of .text.boot), NOT the ELF entry — so _start must be first and set sp.
 *
 * This firmware uses hardware floating point (the FFT butterflies). On a bare-
 * metal RISC-V boot the FPU is OFF (mstatus.FS = 0); the first FP instruction
 * would trap as illegal and, with no trap handler installed, spin forever. So
 * enable the FPU (mstatus.FS = Initial) before calling into C. */
extern char _stack_top[];

__attribute__((naked, used, section(".text.boot"))) void _start(void)
{
    __asm__ volatile(
        "lla sp, _stack_top\n\t"
        "li  t0, 0x2000\n\t"       /* mstatus.FS = 1 (Initial): enable the FPU */
        "csrs mstatus, t0\n\t"
        "call kernel_main\n\t"
        "1:\twfi\n\t"
        "j 1b\n\t");
}
