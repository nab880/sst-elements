/*
 * riscv_virt_gpu_trace.c — GPU MMIO trace test for Quetz P1.
 *
 * Exercises GpuTraceRegionHandler: one doorbell write at offset 0,
 * POLL_COUNT status polls at offset 8.  MMIO window at 0x80100000 must
 * be matched by a gpu_trace region handler (slot 0, before sub_ram).
 *
 * 0x80100000 lies in QEMU virt DRAM; polls read back the last doorbell
 * write without a real GPU device (P2 adds BUSY/IDLE modeling).
 *
 * Build (from firmware/):
 *   riscv64-unknown-linux-musl-gcc \
 *     -march=rv64gc -mabi=lp64d -O2 -mcmodel=medany \
 *     -nostdlib -nostartfiles -ffreestanding -mno-relax \
 *     -T link_rv64.ld -Wl,--build-id=none \
 *     riscv_virt_gpu_trace.c -o riscv_virt_gpu_trace
 */

#define GPU_BASE     0x80100000UL
#define GPU_DOORBELL (*(volatile unsigned long*)(GPU_BASE + 0x00))
#define GPU_STATUS   (*(volatile unsigned long*)(GPU_BASE + 0x08))

#define POLL_COUNT   8

#define UART0_BASE  0x10000000UL
#define UART_THR    (*(volatile unsigned char*)(UART0_BASE + 0x00))
#define UART_LSR    (*(volatile unsigned char*)(UART0_BASE + 0x05))
#define LSR_THRE    (1u << 5)

#define TESTDEV      (*(volatile unsigned int*)0x100000UL)
#define TESTDEV_PASS 0x5555u

static void uart_putc(char c) {
    while (!(UART_LSR & LSR_THRE));
    UART_THR = (unsigned char)c;
}

static void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}

void _start(void) {
    unsigned long sink = 0;

    GPU_DOORBELL = 0x00000000DEADBEEFUL;
    for (int i = 0; i < POLL_COUNT; i++)
        sink ^= GPU_STATUS;
    (void)sink;

    uart_puts("GPU trace done\n");
    TESTDEV = TESTDEV_PASS;
    while (1) __asm__ volatile ("wfi");
}
