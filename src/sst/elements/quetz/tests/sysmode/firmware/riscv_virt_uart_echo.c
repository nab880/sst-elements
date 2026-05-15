/*
 * riscv_virt_uart_echo.c — UART loopback test for QEMU RISC-V virt machine.
 *
 * Reads UART_ECHO_COUNT bytes from UART RX, increments each by 1,
 * and writes them to UART TX.  Appends a newline.  Exits via the
 * SiFive test finisher.
 *
 * The test harness injects input bytes by redirecting QEMU stdin
 * (appstdin parameter).  QEMU virt connects UART0 to stdio with
 * -nographic, so stdin feeds UART RX and UART TX goes to stdout.
 *
 * UART NS16550A at 0x10000000:
 *   0x00: RBR (read) / THR (write)
 *   0x05: LSR — bit 0 = data ready (RX), bit 5 = TX holding empty
 */

#define UART0_BASE  0x10000000UL
#define UART_RBR    (*(volatile unsigned char*)(UART0_BASE + 0x00))
#define UART_THR    (*(volatile unsigned char*)(UART0_BASE + 0x00))
#define UART_LSR    (*(volatile unsigned char*)(UART0_BASE + 0x05))
#define LSR_DR      (1u << 0)   /* data ready */
#define LSR_THRE    (1u << 5)   /* TX holding empty */

#define TESTDEV      (*(volatile unsigned int*)0x100000UL)
#define TESTDEV_PASS 0x5555u

#define UART_ECHO_COUNT 5

static void uart_putc(unsigned char c) {
    while (!(UART_LSR & LSR_THRE));
    UART_THR = c;
}

static unsigned char uart_getc(void) {
    while (!(UART_LSR & LSR_DR));
    return UART_RBR;
}

void _start(void) {
    for (int i = 0; i < UART_ECHO_COUNT; i++) {
        unsigned char c = uart_getc();
        uart_putc(c + 1);
    }
    uart_putc('\n');
    TESTDEV = TESTDEV_PASS;
    while (1) __asm__ volatile ("wfi");
}
