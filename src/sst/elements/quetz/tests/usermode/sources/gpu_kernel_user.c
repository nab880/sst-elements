/*
 * gpu_kernel_user.c — user-mode driver for quetz.QuetzGpuDevice.
 *
 * mmap(MAP_FIXED) at 0x80100000 so QEMU user-mode delivers MMIO loads/stores
 * to the SST plugin.  Three kernel launches with LATENCY_OVERRIDE, matching
 * sysmode riscv_virt_gpu_kernel.c register layout.
 *
 * Build: run build.sh in this directory.
 */

#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

/* Give the GPU device clock time to retire kernels between back-to-back
 * doorbells (user-mode does not spin on STATUS). */
static void gpu_sync_delay(void) {
    for (volatile int i = 0; i < 500000; i++) { }
}

#define GPU_BASE    0x80100000UL
#define GPU_SIZE    4096UL

#define OFF_DOORBELL        0x00
#define OFF_STATUS          0x08
#define OFF_KERNEL_ID       0x10
#define OFF_LATENCY_OVR     0x18

static volatile uint64_t *mmio;

static void launch_kernel(unsigned long latency_cycles) {
    mmio[OFF_LATENCY_OVR / 8] = latency_cycles;
    mmio[OFF_DOORBELL / 8] = 0;
    /* Do not spin on STATUS in user-mode: QuetzGpuDevice read responses are
     * not written back into the guest mmap page (SST models timing only). */
    (void)mmio[OFF_KERNEL_ID / 8];
}

int main(void) {
    mmio = mmap((void *)GPU_BASE, GPU_SIZE, PROT_READ | PROT_WRITE,
                MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (mmio == MAP_FAILED) {
        const char msg[] = "mmap fail\n";
        write(2, msg, sizeof msg - 1);
        _exit(1);
    }

    launch_kernel(1000);
    gpu_sync_delay();
    launch_kernel(5000);
    gpu_sync_delay();
    launch_kernel(20000);
    gpu_sync_delay();

    write(1, "done\n", 5);
    _exit(0);
}
