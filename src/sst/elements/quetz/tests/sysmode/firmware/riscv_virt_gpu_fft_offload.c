/*
 * riscv_virt_gpu_fft_offload.c — 256-pt FFT computed ON THE DEVICE (not the CPU).
 * QuetzGpuDevice with quetz.FFTKernel: the guest fills the input, points the
 * device at the buffers (REG_FFT_IN_ADDR/OUT_ADDR/N), rings the doorbell, and
 * reads the result;
 * the device DMAs + computes in C++ so the guest issues ~no compute (the test
 * asserts that). Buffers are LE float32 cfloat[N] in the SST window at 0x90000000.
 */

#include <stdint.h>

#define UART0_BASE   0x10000000UL
#define UART_THR     (*(volatile unsigned char*)(UART0_BASE + 0x00))
#define UART_LSR     (*(volatile unsigned char*)(UART0_BASE + 0x05))
#define LSR_THRE     (1u << 5)
#define TESTDEV      (*(volatile unsigned int*)0x100000UL)
#define TESTDEV_PASS 0x5555u
#define TESTDEV_FAIL 0x3333u

#define GPU_BASE          0x80100000UL
#define GPU_DOORBELL      (*(volatile unsigned long*)(GPU_BASE + 0x00))
#define GPU_STATUS        (*(volatile unsigned long*)(GPU_BASE + 0x08))
#define GPU_FFT_IN_ADDR   (*(volatile unsigned long*)(GPU_BASE + 0x30))
#define GPU_FFT_OUT_ADDR  (*(volatile unsigned long*)(GPU_BASE + 0x38))
#define GPU_FFT_N         (*(volatile unsigned long*)(GPU_BASE + 0x40))

#define FFT_N   256u

/* FFT buffers in the SST-backed window (0x90000000). float2 = {re, im}. */
typedef struct { float re, im; } cfloat;
#define FFT_WIN   0x90000000UL
static volatile cfloat *const fft_in  = (volatile cfloat *)(FFT_WIN + 0x0000);
static volatile cfloat *const fft_out = (volatile cfloat *)(FFT_WIN + 0x1000);

static void uart_putc(char c) { while (!(UART_LSR & LSR_THRE)); UART_THR = (unsigned char)c; }
static void uart_puts(const char *s) { while (*s) uart_putc(*s++); }
static void uart_put_u32_dec(uint32_t v) {
    char b[12]; unsigned i = 0;
    if (!v) { uart_putc('0'); return; }
    while (v && i < sizeof(b)) { b[i++] = (char)('0' + v % 10u); v /= 10u; }
    while (i) uart_putc(b[--i]);
}

void kernel_main(void)
{
    uart_puts("GPU FFT offload (device-computed)\n");

    /* impulse: x[0] = 1+0j, rest 0 — written into the SST-backed window */
    for (uint32_t i = 0; i < FFT_N; i++) {
        fft_in[i].re = (i == 0) ? 1.0f : 0.0f;
        fft_in[i].im = 0.0f;
    }

    /* program the device and fire — the DEVICE does the FFT (no butterflies here) */
    GPU_FFT_IN_ADDR  = (unsigned long)FFT_WIN + 0x0000;
    GPU_FFT_OUT_ADDR = (unsigned long)FFT_WIN + 0x1000;
    GPU_FFT_N        = FFT_N;
    GPU_DOORBELL     = 0;               /* blocking: returns when result is ready */

    /* read the device's result back and verify impulse -> 1+0j everywhere */
    uint32_t correct = 0;
    for (uint32_t i = 0; i < FFT_N; i++) {
        if (fft_out[i].re == 1.0f) correct++;
        if (fft_out[i].im == 0.0f) correct++;
    }

    uart_puts("GPU FFT offload correct_words=");
    uart_put_u32_dec(correct);
    uart_putc('/');
    uart_put_u32_dec(2u * FFT_N);
    uart_putc('\n');

    TESTDEV = (correct == 2u * FFT_N) ? TESTDEV_PASS : TESTDEV_FAIL;
    while (1)
        __asm__ volatile ("wfi");
}

extern char _stack_top[];
__attribute__((naked, used, section(".text.boot"))) void _start(void)
{
    __asm__ volatile(
        "lla sp, _stack_top\n\t"
        "li  t0, 0x2000\n\t"       /* mstatus.FS = 1: enable FPU (float compares) */
        "csrs mstatus, t0\n\t"
        "call kernel_main\n\t"
        "1:\twfi\n\t"
        "j 1b\n\t");
}
