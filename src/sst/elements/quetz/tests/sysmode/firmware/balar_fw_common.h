

#ifndef BALAR_FW_COMMON_H
#define BALAR_FW_COMMON_H

#include <stddef.h>
#include <stdint.h>


#define BALAR_DOORBELL 0x70000000UL
#define UART0_BASE     0x10000000UL
#define UART_THR       (*(volatile unsigned char*)(UART0_BASE + 0x00))
#define UART_LSR       (*(volatile unsigned char*)(UART0_BASE + 0x05))
#define LSR_THRE       (1u << 5)
#define TESTDEV        (*(volatile unsigned int*)0x100000UL)
#define TESTDEV_PASS   0x5555u
#define TESTDEV_FAIL   0x3333u


static inline void uart_putc(char c)
{
    while (!(UART_LSR & LSR_THRE))
        ;
    UART_THR = (unsigned char)c;
}

static inline void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

static inline void uart_put_u64_dec(uint64_t v)
{
    char buf[32];
    unsigned i = 0;
    if (v == 0) {
        uart_putc('0');
        return;
    }
    while (v && i < sizeof(buf)) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i)
        uart_putc(buf[--i]);
}


static inline void *fw_memset(void *dst, int val, size_t n)
{
    uint8_t *p = (uint8_t*)dst;
    while (n--)
        *p++ = (uint8_t)val;
    return dst;
}

static inline void *fw_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t*)dst;
    const uint8_t *s = (const uint8_t*)src;
    while (n--)
        *d++ = *s++;
    return dst;
}

static inline void fw_strcpy(char *dst, const char *src, size_t cap)
{
    if (cap == 0)
        return;
    while (cap > 1 && *src) {
        *dst++ = *src++;
        cap--;
    }
    *dst = '\0';
}


static inline void mmio_write64(uint64_t addr, uint64_t value)
{
    *(volatile uint64_t*)(uintptr_t)addr = value;
}

static inline uint64_t mmio_read64(uint64_t addr)
{
    return *(volatile uint64_t*)(uintptr_t)addr;
}


static inline uint64_t balar_issue_packet(uint8_t *scratch, size_t scratch_bytes,
                                          BalarCudaCallPacket_t *pkt, size_t extra_bytes)
{
    size_t total = sizeof(*pkt) + extra_bytes;
    if (total > scratch_bytes) {
        uart_puts("balar scratch overflow\n");
        TESTDEV = TESTDEV_FAIL;
        while (1)
            __asm__ volatile ("wfi");
    }
    fw_memcpy(scratch, pkt, sizeof(*pkt));
    mmio_write64(BALAR_DOORBELL, (uint64_t)(uintptr_t)scratch);
    return mmio_read64(BALAR_DOORBELL);
}

#endif 
