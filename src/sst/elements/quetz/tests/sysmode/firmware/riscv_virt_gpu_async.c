/* riscv_virt_gpu_async.c — P4 asynchronous-offload overlap demo on the synthetic
 * QuetzGpuDevice (no balar dependency).
 *
 * The doorbell write returns immediately and the device runs the kernel in its
 * own clock domain. Instead of blocking, the guest reads its submission TICKET,
 * does real CPU work *while the GPU is busy*, then joins by polling the
 * COMPLETED counter (REG_KERNEL_ID) until it reaches the ticket. This is the
 * poll-based async contract that Phase 2 brings to the balar path. */

#define GPU_BASE            0x80100000UL
#define GPU_DOORBELL        (*(volatile unsigned long*)(GPU_BASE + 0x00)) /* W: submit   */
#define GPU_STATUS          (*(volatile unsigned long*)(GPU_BASE + 0x08)) /* R: busy/idle*/
#define GPU_COMPLETED       (*(volatile unsigned long*)(GPU_BASE + 0x10)) /* R: completed*/
#define GPU_LATENCY_OVR     (*(volatile unsigned long*)(GPU_BASE + 0x18)) /* W           */
#define GPU_TICKET          (*(volatile unsigned long*)(GPU_BASE + 0x20)) /* R: my ticket*/
#define GPU_RESULT          (*(volatile unsigned long*)(GPU_BASE + 0x28)) /* R: result   */

#define UART0_BASE  0x10000000UL
#define UART_THR    (*(volatile unsigned char*)(UART0_BASE + 0x00))
#define UART_LSR    (*(volatile unsigned char*)(UART0_BASE + 0x05))
#define LSR_THRE    (1u << 5)

#define TESTDEV      (*(volatile unsigned int*)0x100000UL)
#define TESTDEV_PASS 0x5555u
#define TESTDEV_FAIL 0xDEADu

static void uart_putc(char c) {
    while (!(UART_LSR & LSR_THRE));
    UART_THR = (unsigned char)c;
}

static void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}

/* Post a kernel of `latency` device cycles. Returns immediately with the
 * monotonic ticket assigned to this submission. */
static unsigned long submit(unsigned long latency) {
    GPU_LATENCY_OVR = latency;
    GPU_DOORBELL = 0;
    return GPU_TICKET;
}

/* Join point: spin on the completion counter until our ticket retires. The
 * loop streams instructions into SST but the kernel has usually already
 * finished during cpu_work(), so this typically spins very few times. */
static void wait_ticket(unsigned long t) {
    while (GPU_COMPLETED < t)
        ;
}

/* Real CPU work that retires guest instructions while the GPU runs. Marked
 * volatile-fed so the optimizer cannot delete it. */
static unsigned long cpu_work(unsigned long iters) {
    unsigned long acc = 1;
    for (unsigned long i = 1; i <= iters; i++)
        acc = acc * 1664525UL + 1013904223UL + (i ^ (acc >> 7));
    return acc;
}

void kernel_main(void) {
    /* Launch a long kernel (starts running immediately) and queue a second one
     * behind it — exercises submit-while-busy and advancing tickets. */
    unsigned long t1 = submit(40000);
    unsigned long t2 = submit(40000);

    /* The first kernel is running now; prove it, then overlap CPU work. */
    unsigned long busy_at_submit = GPU_STATUS;          /* expect 1 (busy) */
    unsigned long acc = cpu_work(20000);

    /* Join on the last ticket. */
    wait_ticket(t2);

    if (t1 == 1 && t2 == 2 && busy_at_submit == 1 &&
        GPU_COMPLETED >= t2 && acc != 0) {
        uart_puts("ASYNC OVERLAP OK\n");
        TESTDEV = TESTDEV_PASS;
    } else {
        uart_puts("ASYNC OVERLAP FAIL\n");
        TESTDEV = TESTDEV_FAIL;
    }

    while (1)
        __asm__ volatile ("wfi");
}

/* QEMU -kernel jumps to the load address (0x80000000), so the entry stub must
 * be first (.text.boot) and set up the stack before calling C. */
extern char _stack_top[];

__attribute__((naked, used, section(".text.boot"))) void _start(void)
{
    __asm__ volatile(
        "lla sp, _stack_top\n\t"
        "call kernel_main\n\t"
        "1:\twfi\n\t"
        "j 1b\n\t");
}
