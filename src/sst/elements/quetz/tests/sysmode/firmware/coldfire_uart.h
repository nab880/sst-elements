

#ifndef COLDFIRE_UART_H
#define COLDFIRE_UART_H

#include <stdint.h>

#define UART0_BASE   0xfc060000UL
#define UART_SR      (*(volatile uint8_t*)(UART0_BASE + 0x04))
#define UART_UCR     (*(volatile uint8_t*)(UART0_BASE + 0x08))
#define UART_TB      (*(volatile uint8_t*)(UART0_BASE + 0x0c))
#define UART_RB      (*(volatile uint8_t*)(UART0_BASE + 0x0c))
#define SR_RXRDY     0x01u
#define SR_TXRDY     0x04u

/* UART1 (same programming model, second -serial slot) — e.g. a dedicated GPS
 * feed via `-serial pipe:...` while UART0 stays the console. */
#define UART1_BASE   0xfc064000UL
#define UART1_SR     (*(volatile uint8_t*)(UART1_BASE + 0x04))
#define UART1_UCR    (*(volatile uint8_t*)(UART1_BASE + 0x08))
#define UART1_RB     (*(volatile uint8_t*)(UART1_BASE + 0x0c))

#define TESTDEV      (*(volatile uint32_t*)0x80000000UL)
#define TESTDEV_PASS 0x5555u
#define TESTDEV_FAIL 0x3333u

static inline void uart_init(void)
{
    UART_UCR = 0x20;  
    UART_UCR = 0x30;  
    UART_UCR = 0x05;  
}

static inline void uart_putc(char c)
{
    while (!(UART_SR & SR_TXRDY))
        ;
    UART_TB = (uint8_t)c;
}

static inline void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

static inline char uart_getc(void)
{
    while (!(UART_SR & SR_RXRDY))
        ;
    return (char)UART_RB;
}

static inline void uart1_init(void)
{
    UART1_UCR = 0x20;
    UART1_UCR = 0x30;
    UART1_UCR = 0x05;
}

static inline void uart_put_u32_dec(uint32_t v)
{
    char buf[12];
    unsigned i = 0;
    if (v == 0) {
        uart_putc('0');
        return;
    }
    while (v && i < sizeof(buf)) {
        buf[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (i)
        uart_putc(buf[--i]);
}

static inline void uart_put_u32_hex(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    uart_puts("0x");
    for (int s = 28; s >= 0; s -= 4)
        uart_putc(hex[(v >> s) & 0xfu]);
}


static inline void testdev_done(uint32_t code)
{
    TESTDEV = code;
}

#endif 
