/* riscv_virt_gpu_async_queue.c — P4 completion-queue test (N in-flight).
 *
 * Posts three offloads back-to-back through the Quetz async aperture without
 * waiting between them (async_completion_depth >= 3), then joins on the last
 * ticket. The engine forwards them to the synthetic GPU (doorbell_blocking) one
 * at a time and the COMPLETED counter advances in FIFO order. Each in-flight op
 * uses its own packet buffer (async buffer-lifetime rule). */

#define ASYNC_BASE      0x80100100UL
#define ASYNC_SUBMIT    (*(volatile unsigned long*)(ASYNC_BASE + 0x00))
#define ASYNC_STATUS    (*(volatile unsigned long*)(ASYNC_BASE + 0x08))
#define ASYNC_COMPLETED (*(volatile unsigned long*)(ASYNC_BASE + 0x10))
#define ASYNC_TICKET    (*(volatile unsigned long*)(ASYNC_BASE + 0x20))

#define UART0_BASE  0x10000000UL
#define UART_THR    (*(volatile unsigned char*)(UART0_BASE + 0x00))
#define UART_LSR    (*(volatile unsigned char*)(UART0_BASE + 0x05))
#define LSR_THRE    (1u << 5)

#define TESTDEV      (*(volatile unsigned int*)0x100000UL)
#define TESTDEV_PASS 0x5555u
#define TESTDEV_FAIL 0xDEADu

/* One dedicated packet buffer per in-flight op (kept alive until completion). */
static unsigned char buf0[128] __attribute__((aligned(64)));
static unsigned char buf1[128] __attribute__((aligned(64)));
static unsigned char buf2[128] __attribute__((aligned(64)));

static void uart_putc(char c) {
    while (!(UART_LSR & LSR_THRE));
    UART_THR = (unsigned char)c;
}
static void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}

static unsigned long submit_async(void *pkt) {
    ASYNC_SUBMIT = (unsigned long)pkt;
    return ASYNC_TICKET;
}
static void wait_async(unsigned long t) {
    while (ASYNC_COMPLETED < t)
        ;
}
static void stage(unsigned char *b) {
    for (int i = 0; i < 128; i++)
        b[i] = (unsigned char)(i + 1);
}
static unsigned long cpu_work(unsigned long iters) {
    unsigned long acc = 1;
    for (unsigned long i = 1; i <= iters; i++)
        acc = acc * 1664525UL + 1013904223UL + (i ^ (acc >> 7));
    return acc;
}

void kernel_main(void) {
    stage(buf0);
    stage(buf1);
    stage(buf2);

    /* Post three ops without waiting — the engine queues 2 and 3 behind 1. */
    unsigned long t0 = submit_async(buf0);
    unsigned long t1 = submit_async(buf1);
    unsigned long t2 = submit_async(buf2);
    unsigned long inflight = ASYNC_STATUS;     /* expect 3 outstanding */

    unsigned long acc = cpu_work(20000);
    wait_async(t2);                            /* join on the last ticket */

    if (t0 == 1 && t1 == 2 && t2 == 3 && inflight == 3 &&
        ASYNC_COMPLETED >= 3 && acc != 0) {
        uart_puts("ASYNC QUEUE OK\n");
        TESTDEV = TESTDEV_PASS;
    } else {
        uart_puts("ASYNC QUEUE FAIL\n");
        TESTDEV = TESTDEV_FAIL;
    }

    while (1)
        __asm__ volatile ("wfi");
}

extern char _stack_top[];

__attribute__((naked, used, section(".text.boot"))) void _start(void)
{
    __asm__ volatile(
        "lla sp, _stack_top\n\t"
        "call kernel_main\n\t"
        "1:\twfi\n\t"
        "j 1b\n\t");
}
