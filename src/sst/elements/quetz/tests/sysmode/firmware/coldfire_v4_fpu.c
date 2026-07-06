/*
 * coldfire_v4_fpu.c — ColdFire V4e codegen smoke (FPU + ISA_B + EMAC), the
 * supported-parts gate for V4 targets. Compiled -mcpu=5475 (hard-float V4e), so it
 * emits real FPU/ISA_B/EMAC ops; MUST run with `-cpu cfv4e` or it takes an
 * illegal-instruction/FP-unavailable trap. FP checks are bit-exact.
 * SDL: sysmode/basic_quetz_sysmode.py.
 */

#include <stdint.h>

#include "coldfire_uart.h"

/* volatile blocks constant folding: force runtime FPU instructions */
static volatile double vd_a = 3.0, vd_b = 4.0;
static volatile float  vf_x = 1.5f, vf_y = 2.0f;
static volatile int32_t vi_n = 7;
static volatile uint8_t vb_neg = 0x80;

/* One EMAC multiply-accumulate: MACSR reset state is signed-integer mode,
 * so acc0 = x*y for small operands; movclr reads and clears it. */
static uint32_t emac_mac32(uint32_t x, uint32_t y)
{
    uint32_t r;
    __asm__ volatile(
        "mac.l %1, %2\n\t"
        "movclr.l %%acc0, %0"
        : "=d"(r)
        : "d"(x), "d"(y)
        : "cc");
    return r;
}

void kernel_main(void)
{
    uart_init();
    uart_puts("ColdFire V4 smoke: FPU + ISA_B + EMAC (cfv4e)\n");

    /* FPU, double precision: 3^2 + 4^2 = 25, 25/4 = 6.25 — both exact. */
    double c = vd_a * vd_a + vd_b * vd_b;
    double q = c / vd_b;
    int fpu_ok = (c == 25.0) && (q == 6.25);

    /* FPU, single precision + int<->double conversion (hardware fmove). */
    float f = vf_x * vf_y;                      /* 3.0f exact */
    double d7 = (double)vi_n;                   /* 7.0 */
    int back = (int)(d7 * vd_b);                /* 28 */
    int conv_ok = (f == 3.0f) && (back == 28);

    /* ISA_B byte sign/zero extension (mvs.b / mvz.b codegen). */
    int se = (int)(int8_t)vb_neg;               /* -128 */
    int zx = (int)vb_neg;                       /* 128 */
    int isab_ok = (se == -128) && (zx == 128);

    /* EMAC: 3 * 5 accumulated into acc0. */
    uint32_t mac = emac_mac32(3u, 5u);
    int emac_ok = (mac == 15u);

    uart_puts("fpu: double=");
    uart_puts(fpu_ok ? "ok" : "BAD");
    uart_puts(" conv=");
    uart_puts(conv_ok ? "ok" : "BAD");
    uart_putc('\n');
    uart_puts("isa_b: ext=");
    uart_puts(isab_ok ? "ok" : "BAD");
    uart_puts(" emac: mac32(3,5)=");
    uart_put_u32_dec(mac);
    uart_putc('\n');

    int pass = fpu_ok && conv_ok && isab_ok && emac_ok;
    uart_puts(pass ? "V4 SMOKE PASS\n" : "V4 SMOKE FAIL\n");

    testdev_done(pass ? TESTDEV_PASS : TESTDEV_FAIL);
}
