/* riscv_virt_gpu_async_engine.c — exercises the Quetz async-offload ENGINE
 * (quetzcpu_mmio_sync.cc handleAsyncAperture) against the synthetic GPU device
 * running in doorbell_blocking mode.
 *
 * Unlike riscv_virt_gpu_async.c (which talks to the device's own registers),
 * here the guest posts to a Quetz-emulated submit aperture: Quetz assigns a
 * ticket, drains+flushes the packet, forwards a balar-style doorbell to the
 * device, and acks the SUBMIT. The device holds its write response until the
 * kernel retires, so the COMPLETED counter the guest polls only advances at
 * true kernel completion — modelling a posted balar offload with no balar. */

#define ASYNC_BASE      0x80100100UL
#define ASYNC_SUBMIT    (*(volatile unsigned long*)(ASYNC_BASE + 0x00)) /* W: scratch  */
#define ASYNC_STATUS    (*(volatile unsigned long*)(ASYNC_BASE + 0x08)) /* R: in-flight*/
#define ASYNC_COMPLETED (*(volatile unsigned long*)(ASYNC_BASE + 0x10)) /* R: completed*/
#define ASYNC_TICKET    (*(volatile unsigned long*)(ASYNC_BASE + 0x20)) /* R: my ticket*/
#define ASYNC_RESULT    (*(volatile unsigned long*)(ASYNC_BASE + 0x28)) /* R: result   */

#define UART0_BASE  0x10000000UL
#define UART_THR    (*(volatile unsigned char*)(UART0_BASE + 0x00))
#define UART_LSR    (*(volatile unsigned char*)(UART0_BASE + 0x05))
#define LSR_THRE    (1u << 5)

#define TESTDEV      (*(volatile unsigned int*)0x100000UL)
#define TESTDEV_PASS 0x5555u
#define TESTDEV_FAIL 0xDEADu

static unsigned char scratch[256] __attribute__((aligned(64)));

static void uart_putc(char c) {
    while (!(UART_LSR & LSR_THRE));
    UART_THR = (unsigned char)c;
}
static void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}

/* Post the scratch-packet address; returns immediately with our ticket. */
static unsigned long submit_async(void *pkt) {
    ASYNC_SUBMIT = (unsigned long)pkt;
    return ASYNC_TICKET;
}
static void wait_async(unsigned long t) {
    while (ASYNC_COMPLETED < t)
        ;
}
static unsigned long cpu_work(unsigned long iters) {
    unsigned long acc = 1;
    for (unsigned long i = 1; i <= iters; i++)
        acc = acc * 1664525UL + 1013904223UL + (i ^ (acc >> 7));
    return acc;
}

void kernel_main(void) {
    /* Stage a packet; these writes must drain before Quetz flushes + forwards. */
    for (int i = 0; i < 256; i++)
        scratch[i] = (unsigned char)i;

    /* Post op 1, prove it is in flight, overlap CPU work, then join. */
    unsigned long t1   = submit_async(scratch);
    unsigned long busy = ASYNC_STATUS;          /* expect >= 1 in flight */
    unsigned long acc  = cpu_work(20000);
    wait_async(t1);

    /* Post op 2 (engine reused after the first retired) and join. */
    unsigned long t2 = submit_async(scratch);
    wait_async(t2);

    if (t1 == 1 && t2 == 2 && busy >= 1 &&
        ASYNC_COMPLETED >= 2 && acc != 0) {
        uart_puts("ASYNC ENGINE OK\n");
        TESTDEV = TESTDEV_PASS;
    } else {
        uart_puts("ASYNC ENGINE FAIL\n");
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
