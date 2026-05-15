/*
 * riscv_virt_hello.c — bare-metal hello-world for QEMU RISC-V virt machine.
 *
 * UART:   NS16550A at 0x10000000 (QEMU virt default).
 *         LSR (0x05): bit 5 = TX holding register empty → ready to transmit.
 *         THR (0x00): write byte to transmit.
 * Exit:   SiFive test finisher at 0x100000 — write 0x5555 to exit with PASS.
 *
 * Build (from firmware/):
 *   riscv64-unknown-linux-musl-gcc \
 *     -march=rv64gc -mabi=lp64d -O2 -mcmodel=medany \
 *     -nostdlib -nostartfiles -ffreestanding -mno-relax \
 *     -T link_rv64.ld -Wl,--build-id=none \
 *     riscv_virt_hello.c -o riscv_virt_hello
 */

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
    uart_puts("Hello from RISC-V virt!\n");
    TESTDEV = TESTDEV_PASS;
    while (1) __asm__ volatile ("wfi");
}
