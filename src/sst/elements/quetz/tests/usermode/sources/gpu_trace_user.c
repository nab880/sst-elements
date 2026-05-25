/*
 * gpu_trace_user.c — user-mode GpuTraceRegionHandler exercise.
 *
 * One doorbell write (0xDEADBEEF) and POLL_COUNT status reads, matching
 * sysmode riscv_virt_gpu_trace.c behavior.
 *
 * Build: run build.sh in this directory.
 */

#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#define GPU_BASE    0x80100000UL
#define GPU_SIZE    4096UL
#define POLL_COUNT  8

#define OFF_DOORBELL  0x00
#define OFF_STATUS    0x08

static volatile uint64_t *mmio;

int main(void) {
    unsigned long sink = 0;
    int i;

    mmio = mmap((void *)GPU_BASE, GPU_SIZE, PROT_READ | PROT_WRITE,
                MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (mmio == MAP_FAILED) {
        const char msg[] = "mmap fail\n";
        write(2, msg, sizeof msg - 1);
        _exit(1);
    }

    mmio[OFF_DOORBELL / 8] = 0x00000000DEADBEEFUL;
    for (i = 0; i < POLL_COUNT; i++)
        sink ^= mmio[OFF_STATUS / 8];
    (void)sink;

    _exit(0);
}
