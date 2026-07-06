/*
 * coldfire_accel_sink.c — full loop on one guest: stimulus (QuetzStreamDevice)
 * -> device compute (quetz.ScaleOffsetKernel) -> capture (QuetzSinkDevice),
 * ColdFire m68k. The guest also CPU-cross-checks the device output so a mismatch
 * is attributed (device vs capture path). u32 moves = 4 LE stream bytes via SST's
 * value-serialization, no BE swapping. SDL: basic_quetz_gpu_compute_coldfire.py.
 */

#include <stdint.h>

#include "coldfire_uart.h"

#define GPU_BASE          0x70000000UL
#define GPU_DOORBELL      (GPU_BASE + 0x00UL)   /* W: submit (blocking) */
#define GPU_STATUS        (GPU_BASE + 0x08UL)   /* R: busy(1)/idle(0) */
#define GPU_ARG0          (GPU_BASE + 0x30UL)   /* W: input  s16[N] addr */
#define GPU_ARG1          (GPU_BASE + 0x38UL)   /* W: output s16[N] addr */
#define GPU_ARG2          (GPU_BASE + 0x40UL)   /* W: sample count N */
#define GPU_ARG3          (GPU_BASE + 0x48UL)   /* W: scale | offset<<16 */

#define SENSOR_BASE       0x70010000UL
#define SENSOR_STATUS     (SENSOR_BASE + 0x00UL)
#define SENSOR_DATA       (SENSOR_BASE + 0x08UL)
#define SENSOR_SEQ        (SENSOR_BASE + 0x10UL)
#define SENSOR_EOS        (SENSOR_BASE + 0x20UL)

#define SINK_BASE         0x70020000UL
#define SINK_STATUS       (SINK_BASE + 0x00UL)  /* R: bytes accepted */
#define SINK_DATA         (SINK_BASE + 0x08UL)  /* W: push write-size bytes */
#define SINK_CTRL         (SINK_BASE + 0x18UL)  /* W: 1 = flush to file */

#define SCALE             3
#define OFFSET            500

#define MAX_WORDS         64u                   /* fixture is 256 bytes */

#define WIN               0x71000000UL
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

/* Mirror of quetz_scale_offset.h:quetz_scale_offset_sat16(). */
static int16_t expect(int16_t s)
{
    int32_t v = (int32_t)s * SCALE + OFFSET;
    if (v > 32767)  return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/* Transform one packed word (two LE s16 samples) the way the device does. */
static uint32_t expect_word(uint32_t w)
{
    int16_t s0 = (int16_t)(uint16_t)(w & 0xFFFFu);
    int16_t s1 = (int16_t)(uint16_t)(w >> 16);
    return (uint32_t)(uint16_t)expect(s0)
         | ((uint32_t)(uint16_t)expect(s1) << 16);
}

void kernel_main(void)
{
    static uint32_t words[MAX_WORDS];

    uart_init();
    uart_puts("ColdFire accel->sink loop: stream in, device compute, capture out\n");

    /* 1. Drain the recorded stream (unpaced: all bytes ready at t=0). */
    uint32_t nwords = 0;
    while (!mmio_read32(SENSOR_EOS) && nwords < MAX_WORDS)
        words[nwords++] = mmio_read32(SENSOR_DATA);

    uint32_t total = mmio_read32(SENSOR_SEQ);
    int fixture_ok = (nwords >= 2) && (total == nwords * 4);

    /* Verify the sum32 trailer (last word) over the payload bytes. */
    if (fixture_ok) {
        uint32_t sum = 0;
        for (uint32_t i = 0; i + 1 < nwords; i++) {
            uint32_t w = words[i];
            sum += (w & 0xFFu) + ((w >> 8) & 0xFFu)
                 + ((w >> 16) & 0xFFu) + ((w >> 24) & 0xFFu);
        }
        fixture_ok = (sum == words[nwords - 1]);
    }
    uart_puts("stream: words=");
    uart_put_u32_dec(nwords);
    uart_puts(" trailer=");
    uart_puts(fixture_ok ? "ok" : "BAD");
    uart_putc('\n');

    /* 2. Stage payload samples in the window and transform ON THE DEVICE. */
    uint32_t payload_words = nwords - 1;          /* trailer stays out */
    uint32_t nsamples = payload_words * 2;
    for (uint32_t i = 0; i < payload_words; i++)
        buf_in[i] = words[i];

    mmio_write32(GPU_ARG0, (uint32_t)WIN + 0x0000u);
    mmio_write32(GPU_ARG1, (uint32_t)WIN + 0x1000u);
    mmio_write32(GPU_ARG2, nsamples);
    mmio_write32(GPU_ARG3, (uint32_t)(uint16_t)SCALE
                         | ((uint32_t)(uint16_t)OFFSET << 16));
    mmio_write32(GPU_DOORBELL, 0);                /* blocking: returns when done */

    /* 3. Cross-check the device result, then push it into the sink. */
    uint32_t correct = 0;
    for (uint32_t i = 0; i < payload_words; i++) {
        uint32_t w = buf_out[i];
        if (w == expect_word(words[i]))
            correct += 2;
        mmio_write32(SINK_DATA, w);               /* u32 store = 4 bytes */
    }
    mmio_write32(SINK_CTRL, 1);                   /* flush capture to file */

    uint32_t captured = mmio_read32(SINK_STATUS);

    uart_puts("accel: correct_samples=");
    uart_put_u32_dec(correct);
    uart_putc('/');
    uart_put_u32_dec(nsamples);
    uart_puts(" sink_bytes=");
    uart_put_u32_dec(captured);
    uart_putc('\n');

    int pass = fixture_ok && correct == nsamples && captured == nsamples * 2;
    uart_puts(pass ? "SINK LOOP PASS\n" : "SINK LOOP FAIL\n");

    testdev_done(pass ? TESTDEV_PASS : TESTDEV_FAIL);
}
