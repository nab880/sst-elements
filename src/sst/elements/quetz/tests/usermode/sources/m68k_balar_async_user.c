/*
 * m68k_balar_async_user.c — P6 Phase 3 big-endian user-mode ASYNC balar offload.
 *
 * Same as m68k_balar_user.c but the post-launch cudaThreadSynchronize is posted
 * via the Quetz async aperture (BALAR_ASYNC_BASE = doorbell+0x100): the guest
 * runs CPU work while balar runs the kernel, then joins on the ticket. Exercises
 * the P4 async engine end-to-end through the P6 user-mode MMIO trap (the async
 * submit/ticket/completed registers also fault into the SIGSEGV handler).
 * Requires QUETZ_ASYNC_OFFLOAD=1 on the SDL. Result must match the sync path.
 */

#include "coldfire_balar.h"

static long sys_write(int fd, const void *buf, unsigned long n)
{
    register long d0 __asm__("d0") = 4;
    register long d1 __asm__("d1") = fd;
    register long d2 __asm__("d2") = (long)buf;
    register long d3 __asm__("d3") = (long)n;
    __asm__ volatile("trap #0" : "+d"(d0) : "d"(d1), "d"(d2), "d"(d3) : "memory");
    return d0;
}

static void sys_exit(int code)
{
    register long d0 __asm__("d0") = 1;
    register long d1 __asm__("d1") = code;
    __asm__ volatile("trap #0" : "+d"(d0) : "d"(d1));
    __builtin_unreachable();
}

static void put_result(uint32_t correct)
{
    char buf[64];
    int n = 0;
    const char *k = "Balar vectorAdd async user-mode correct=";
    while (*k) {
        buf[n++] = *k++;
    }
    char tmp[12];
    int t = 0;
    uint32_t v = correct;
    if (v == 0) {
        tmp[t++] = '0';
    }
    while (v) {
        tmp[t++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (t) {
        buf[n++] = tmp[--t];
    }
    const char *tail = "/256\n";
    while (*tail) {
        buf[n++] = *tail++;
    }
    sys_write(1, buf, n);
}

void _start(void)
{
    uint32_t correct = cb_vadd_async();
    put_result(correct);
    sys_exit(correct == CB_VEC_N ? 0 : 1);
}
