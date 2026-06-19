/*
 * m68k_mmio_roundtrip.c — P6 Phase 2 big-endian user-mode MMIO round-trip.
 *
 * ColdFire/m68k counterpart of rv64_mmio_roundtrip.c: 32-bit MMIO, big-endian.
 * Does not mmap the aperture — qemu-m68k reserves it PROT_NONE (-sst-mmio-range)
 * and the linux-user SIGSEGV handler routes the faulting MOVE to the sync mailbox.
 *
 * Freestanding (-nostdlib): no m68k glibc dev package is installed, so we make
 * Linux syscalls directly (number in d0, args in d1.., trap #0).
 */

typedef unsigned int u32;

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

#define GPU_BASE        0x70000000UL
#define REG_DOORBELL    (0x00 / 4)
#define REG_STATUS      (0x08 / 4)
#define REG_KERNEL_ID   (0x10 / 4)

static void put_kv(const char *k, u32 v)
{
    char buf[40];
    int n = 0;
    while (*k) {
        buf[n++] = *k++;
    }
    char tmp[12];
    int t = 0;
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
    buf[n++] = '\n';
    sys_write(1, buf, n);
}

void _start(void)
{
    volatile u32 *mmio = (volatile u32 *)GPU_BASE;

    u32 status0 = mmio[REG_STATUS];    /* load round-trip (idle == 0) */
    mmio[REG_DOORBELL] = 1;            /* store round-trip (submit)   */
    u32 kid = mmio[REG_KERNEL_ID];     /* load round-trip (counter)   */

    put_kv("status0=", status0);
    put_kv("kernel_id=", kid);
    sys_write(1, "roundtrip_done\n", 15);
    sys_exit(0);
}
