/* Malformed kernel DMA must reject safely; a subsequent valid operation must still complete. */

#include <stdint.h>

#include "coldfire_uart.h"
#include "coldfire_devices.h"
#include "coldfire_scale_ref.h"

#define WIN               0x71000000UL
#define IN_ADDR           (WIN + 0x0000UL)
#define OUT_ADDR          (WIN + 0x1000UL)
#define EDGE_ADDR         (WIN + 0xFF00UL)   /* 256 bytes below window end */
#define WILD_ADDR         0x20000000UL

#define N_VALID           64u
#define SCALE             2
#define OFFSET            100

/* Poll completion in either submit mode, including late output-range rejection. */
static uint32_t submit(uint32_t src, uint32_t dst, uint32_t n)
{
    mmio_write32(GPU_ARG0, src);
    mmio_write32(GPU_ARG1, dst);
    mmio_write32(GPU_ARG2, n);
    mmio_write32(GPU_ARG3, (uint32_t)(uint16_t)SCALE
                         | ((uint32_t)(uint16_t)OFFSET << 16));
    mmio_write32(GPU_DOORBELL, 0);
    while (mmio_read32(GPU_STATUS) != 0)
        ;
    return mmio_read32(GPU_KERNEL_ID);
}

static int16_t expect(int16_t s)
{
    return cf_scale_offset_ref(s, SCALE, OFFSET);
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
