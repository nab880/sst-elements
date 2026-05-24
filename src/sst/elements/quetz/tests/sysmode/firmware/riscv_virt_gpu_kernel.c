

#define GPU_BASE            0x80100000UL
#define GPU_DOORBELL        (*(volatile unsigned long*)(GPU_BASE + 0x00))
#define GPU_STATUS          (*(volatile unsigned long*)(GPU_BASE + 0x08))
#define GPU_KERNEL_ID       (*(volatile unsigned long*)(GPU_BASE + 0x10))
#define GPU_LATENCY_OVR     (*(volatile unsigned long*)(GPU_BASE + 0x18))

#define UART0_BASE  0x10000000UL
#define UART_THR    (*(volatile unsigned char*)(UART0_BASE + 0x00))
#define UART_LSR    (*(volatile unsigned char*)(UART0_BASE + 0x05))
#define LSR_THRE    (1u << 5)

#define TESTDEV      (*(volatile unsigned int*)0x100000UL)
#define TESTDEV_PASS 0x5555u

static volatile unsigned long last_kernel_id;

static void uart_putc(char c) {
    while (!(UART_LSR & LSR_THRE));
    UART_THR = (unsigned char)c;
}

static void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}

static void launch_kernel(unsigned long latency_cycles) {
    GPU_LATENCY_OVR = latency_cycles;
    GPU_DOORBELL = 0;
    while (GPU_STATUS)
        ;
    last_kernel_id = GPU_KERNEL_ID;
}

void _start(void) {
    launch_kernel(1000);
    launch_kernel(5000);
    launch_kernel(20000);
    uart_puts("GPU kernels done\n");
    TESTDEV = TESTDEV_PASS;
    while (1)
        __asm__ volatile ("wfi");
}
