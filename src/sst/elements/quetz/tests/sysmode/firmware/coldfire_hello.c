

#include <stdint.h>

#define UART0_BASE   0xfc060000UL
#define UART_SR      (*(volatile uint8_t*)(UART0_BASE + 0x04))
#define UART_UCR     (*(volatile uint8_t*)(UART0_BASE + 0x08))  
#define UART_TB      (*(volatile uint8_t*)(UART0_BASE + 0x0c))
#define SR_TXRDY     0x04u

#define TESTDEV      (*(volatile uint32_t*)0x80000000UL)
#define TESTDEV_PASS 0x5555u


static void uart_init(void)
{
    UART_UCR = 0x20;  
    UART_UCR = 0x30;  
    UART_UCR = 0x05;  
}

static void uart_putc(char c)
{
    while (!(UART_SR & SR_TXRDY))
        ;
    UART_TB = (uint8_t)c;
}

static void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}


void kernel_main(void)
{
    uart_init();
    uart_puts("Hello from NXP ColdFire (mcf5208evb / m68k)!\n");
    TESTDEV = TESTDEV_PASS;
}
