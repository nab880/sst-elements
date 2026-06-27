/*
 * rv64_mmio_roundtrip.c — P6 Phase 0 user-mode MMIO round-trip.
 *
 * Unlike the pre-P6 gpu_kernel_user.c, this does NOT mmap the aperture: the
 * Quetz launcher passes -sst-mmio-range, qemu-riscv64 reserves the window
 * PROT_NONE, and each load/store faults into the linux-user SIGSEGV handler,
 * which routes it to the SST sync mailbox (QuetzGpuDevice responds).
 *
 * Build non-compressed (-march=rv64g) so every load/store is a 4-byte insn the
 * sst_mmio decoder understands (RVC support arrives in Phase 1).
 */

#include <stdint.h>
#include <unistd.h>

#define GPU_BASE        0x80100000UL
#define REG_DOORBELL    (0x00 / 8)
#define REG_STATUS      (0x08 / 8)
#define REG_KERNEL_ID   (0x10 / 8)

static void put_kv(const char *k, uint64_t v)
{
    char buf[40];
    int n = 0;
    while (*k) {
        buf[n++] = *k++;
    }
    char tmp[20];
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
    write(1, buf, n);
}

int main(void)
{
    volatile uint64_t *mmio = (volatile uint64_t *)GPU_BASE;

    uint64_t status0 = mmio[REG_STATUS];   /* load round-trip (idle == 0) */
    mmio[REG_DOORBELL] = 1;                 /* store round-trip (submit)   */
    uint64_t kid = mmio[REG_KERNEL_ID];     /* load round-trip (counter)   */

    put_kv("status0=", status0);
    put_kv("kernel_id=", kid);
    write(1, "roundtrip_done\n", 15);
    return 0;
}
