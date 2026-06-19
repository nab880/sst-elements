/*
 * m68k_balar_user.c — P6 Phase 2 big-endian user-mode balar vectorAdd offload.
 *
 * Reuses the system-mode ColdFire marshalling (coldfire_balar.h): big-endian m68k
 * hand-packs the little-endian balar wire packet (st_le*) and rings the doorbell.
 * Here it runs as a freestanding Linux m68k process (no glibc dev package): the
 * doorbell at BALAR_DOORBELL is reserved PROT_NONE by qemu-m68k (-sst-mmio-range)
 * and each MOVE faults into the linux-user SIGSEGV handler -> sync mailbox ->
 * BalarAcceleratorPort (flush + forward). cb_vadd() returns the correct count.
 */

#include "coldfire_balar.h"

static long sys_write(int fd, const void *buf, unsigned long n)
{
    register long d0 __asm__("d0") = 4;            /* __NR_write */
    register long d1 __asm__("d1") = fd;
    register long d2 __asm__("d2") = (long)buf;
    register long d3 __asm__("d3") = (long)n;
    __asm__ volatile("trap #0"
                     : "+d"(d0)
                     : "d"(d1), "d"(d2), "d"(d3)
                     : "memory");
    return d0;
}

static void sys_exit(int code)
{
    register long d0 __asm__("d0") = 1;            /* __NR_exit */
    register long d1 __asm__("d1") = code;
    __asm__ volatile("trap #0" : "+d"(d0) : "d"(d1));
    __builtin_unreachable();
}

static void put_result(uint32_t correct)
{
    char buf[64];
    int n = 0;
    const char *k = "Balar vectorAdd user-mode correct=";
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
    uint32_t correct = cb_vadd();
    put_result(correct);
    sys_exit(correct == CB_VEC_N ? 0 : 1);
}
