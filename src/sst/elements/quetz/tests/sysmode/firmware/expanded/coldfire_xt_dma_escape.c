/*
 * coldfire_xt_dma_escape.c — guest-programmed kernel-op registers must not
 * be able to crash the simulator. Pre-fix, a src/dst outside the SST-backed
 * window sent kernel DMA to an address no memHierarchy endpoint owns (MemNIC
 * routing FATAL, the whole simulation dies), and N=0 hit an out.fatal in the
 * device. Post-fix (dma_range_start/end + non-fatal rejection) each bad op
 * is abandoned: the blocking doorbell completes, REG_KERNEL_ID does not
 * advance, gpu.ops_rejected counts it — and the device still runs a valid
 * op afterwards.
 *
 * Probes (all doorbells blocking; KERNEL_ID checked after each):
 *   1. N=0                                  -> kernel rejects the args
 *   2. src=0x20000000 (wild base)           -> input range reject
 *   3. src=WIN+0xFF00, N=512 (1 KiB)        -> input straddles window end
 *   4. src ok, dst=WIN+0xFF00, N=512        -> OUTPUT straddles window end
 *                                              (rejected at writeback, after
 *                                              the compute+busy phases)
 *   5. valid 64-sample scale=2/offset=100   -> must still work; KERNEL_ID=1
 *
 * SDL: sysmode/basic_quetz_gpu_compute_coldfire.py
 *   (QUETZ_KERNEL=quetz.ScaleOffsetKernel, default LE window).
 */

#include <stdint.h>

#include "coldfire_uart.h"

#define GPU_BASE          0x70000000UL
#define GPU_DOORBELL      (GPU_BASE + 0x00UL)
#define GPU_KERNEL_ID     (GPU_BASE + 0x10UL)
#define GPU_ARG0          (GPU_BASE + 0x30UL)
#define GPU_ARG1          (GPU_BASE + 0x38UL)
#define GPU_ARG2          (GPU_BASE + 0x40UL)
#define GPU_ARG3          (GPU_BASE + 0x48UL)

#define WIN               0x71000000UL
#define IN_ADDR           (WIN + 0x0000UL)
#define OUT_ADDR          (WIN + 0x1000UL)
#define EDGE_ADDR         (WIN + 0xFF00UL)   /* 256 bytes below window end */
#define WILD_ADDR         0x20000000UL

#define N_VALID           64u
#define SCALE             2
#define OFFSET            100

static inline void mmio_write32(uint32_t addr, uint32_t v)
{
    *(volatile uint32_t *)addr = v;
}

static inline uint32_t mmio_read32(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

/* Ring the (blocking) doorbell for src/dst/n; returns KERNEL_ID after. */
static uint32_t submit(uint32_t src, uint32_t dst, uint32_t n)
{
    mmio_write32(GPU_ARG0, src);
    mmio_write32(GPU_ARG1, dst);
    mmio_write32(GPU_ARG2, n);
    mmio_write32(GPU_ARG3, (uint32_t)(uint16_t)SCALE
                         | ((uint32_t)(uint16_t)OFFSET << 16));
    mmio_write32(GPU_DOORBELL, 0);
    return mmio_read32(GPU_KERNEL_ID);
}

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
    uart_puts("ColdFire DMA escape: bad kernel args must not kill the sim (m68k)\n");

    uint32_t kid1 = submit(IN_ADDR, OUT_ADDR, 0);            /* N=0 */
    uint32_t kid2 = submit(WILD_ADDR, OUT_ADDR, N_VALID);    /* wild src */
    uint32_t kid3 = submit(EDGE_ADDR, OUT_ADDR, 512);        /* src straddles */
    uint32_t kid4 = submit(IN_ADDR, EDGE_ADDR, 512);         /* dst straddles */

    uart_puts("rejected probes: kernel_id=");
    uart_put_u32_dec(kid1); uart_putc(',');
    uart_put_u32_dec(kid2); uart_putc(',');
    uart_put_u32_dec(kid3); uart_putc(',');
    uart_put_u32_dec(kid4);
    uart_putc('\n');

    /* Valid op: samples 2i packed two-per-u32 (numeric LE packing). */
    for (uint32_t i = 0; i < N_VALID; i += 2) {
        uint16_t s0 = (uint16_t)(int16_t)(2 * (int32_t)i - 60);
        uint16_t s1 = (uint16_t)(int16_t)(2 * (int32_t)(i + 1) - 60);
        mmio_write32(IN_ADDR + 2 * i, (uint32_t)s0 | ((uint32_t)s1 << 16));
    }
    uint32_t kid5 = submit(IN_ADDR, OUT_ADDR, N_VALID);

    uint32_t correct = 0;
    for (uint32_t i = 0; i < N_VALID; i += 2) {
        uint32_t w = mmio_read32(OUT_ADDR + 2 * i);
        int16_t g0 = (int16_t)(uint16_t)(w & 0xFFFFu);
        int16_t g1 = (int16_t)(uint16_t)(w >> 16);
        if (g0 == expect((int16_t)(2 * (int32_t)i - 60)))       correct++;
        if (g1 == expect((int16_t)(2 * (int32_t)(i + 1) - 60))) correct++;
    }

    uart_puts("valid op after rejects: kernel_id=");
    uart_put_u32_dec(kid5);
    uart_puts(" correct=");
    uart_put_u32_dec(correct);
    uart_putc('/');
    uart_put_u32_dec(N_VALID);
    uart_putc('\n');

    int pass = (kid1 == 0) && (kid2 == 0) && (kid3 == 0) && (kid4 == 0)
            && (kid5 == 1) && (correct == N_VALID);
    uart_puts(pass ? "DMA ESCAPE PASS\n" : "DMA ESCAPE FAIL\n");
    testdev_done(pass ? TESTDEV_PASS : TESTDEV_FAIL);
}
