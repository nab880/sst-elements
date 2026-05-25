

#define MMIO_DOORBELL (*(volatile unsigned long*)0x80100000UL)
#define MMIO_STATUS   (*(volatile unsigned long*)(0x80100000UL + 8))

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
    volatile unsigned long sink;

    MMIO_DOORBELL = 0xDEADBEEFUL;
    sink = MMIO_STATUS;
    (void)sink;
    uart_puts("MMIO poke done\n");
    TESTDEV = TESTDEV_PASS;
    while (1) __asm__ volatile ("wfi");
}
