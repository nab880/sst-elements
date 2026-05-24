/*
 * riscv_virt_mmio_poke.c — bare-metal MMIO poke for Quetz P0 routing test.
 *
 * Writes 0xDEADBEEF to a guest MMIO window at 0x80100000 (outside typical
 * DRAM; must be matched by MmioForwardRegionHandler + mmio_link in SDL).
 *
 * Build (from firmware/):
 *   riscv64-unknown-linux-musl-gcc \
 *     -march=rv64gc -mabi=lp64d -O2 -mcmodel=medany \
 *     -nostdlib -nostartfiles -ffreestanding -mno-relax \
 *     -T link_rv64.ld -Wl,--build-id=none \
 *     riscv_virt_mmio_poke.c -o riscv_virt_mmio_poke
 */

#define MMIO_TEST   (*(volatile unsigned long*)0x80100000UL)

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
    MMIO_TEST = 0xDEADBEEFUL;
    uart_puts("MMIO poke done\n");
    TESTDEV = TESTDEV_PASS;
    while (1) __asm__ volatile ("wfi");
}
