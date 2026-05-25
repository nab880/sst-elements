/*
 * riscv_virt_mmio_poke.c — bare-metal MMIO poke for Quetz P0 routing test.
 *
 * One MMIO write and one MMIO read at 0x80100000 (doorbell + status offset).
 * Traffic must use mmio_link via MmioForwardRegionHandler, not cache_link.
 *
 * Build (from firmware/):
 *   riscv64-unknown-linux-musl-gcc \
 *     -march=rv64gc -mabi=lp64d -O2 -mcmodel=medany \
 *     -nostdlib -nostartfiles -ffreestanding -mno-relax \
 *     -T link_rv64.ld -Wl,--build-id=none \
 *     riscv_virt_mmio_poke.c -o riscv_virt_mmio_poke
 */

#define GPU_BASE            0x80100000UL
#define GPU_DOORBELL        (*(volatile unsigned long*)(GPU_BASE + 0x00))
#define GPU_STATUS          (*(volatile unsigned long*)(GPU_BASE + 0x08))
#define GPU_KERNEL_ID       (*(volatile unsigned long*)(GPU_BASE + 0x10))
#define GPU_LATENCY_OVR     (*(volatile unsigned long*)(GPU_BASE + 0x18))

#define TESTDEV             (*(volatile unsigned int*)0x100000UL)
#define TESTDEV_PASS        0x5555u

static volatile unsigned long last_kernel_id;

static void launch_kernel(unsigned long latency_cycles) {
    GPU_LATENCY_OVR = latency_cycles;
    GPU_DOORBELL = 0;
    while (GPU_STATUS)
        ;
    last_kernel_id = GPU_KERNEL_ID;
}

void _start(void) {
    launch_kernel(1000);
    TESTDEV = TESTDEV_PASS;
    while (1)
        __asm__ volatile ("wfi");
}
