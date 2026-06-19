

#define UART0_BASE   0x40004000U
#define UART_DATA    (*(volatile unsigned int*)(UART0_BASE + 0x00))
#define UART_STATE   (*(volatile unsigned int*)(UART0_BASE + 0x04))
#define UART_CTRL    (*(volatile unsigned int*)(UART0_BASE + 0x08))
#define UART_BAUDDIV (*(volatile unsigned int*)(UART0_BASE + 0x10))

#define TX_FULL (1u << 0)

static void uart_putc(char c) {
    while (UART_STATE & TX_FULL);
    UART_DATA = (unsigned int)(unsigned char)c;
}

static void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}

static void __attribute__((noreturn)) semihost_exit(void) {
    register unsigned int r0 __asm__("r0") = 0x18u;     
    register unsigned int r1 __asm__("r1") = 0x20026u;  
    __asm__ volatile ("bkpt 0xAB" : : "r"(r0), "r"(r1));
    __builtin_unreachable();
}

void uart_main(void) {
    UART_BAUDDIV = 16u;
    UART_CTRL    = 0x1u;    
    uart_puts("Hello from ARM Cortex-M7!\n");
    semihost_exit();
}
