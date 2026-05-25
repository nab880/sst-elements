

#define GPU_BASE            0x80100000UL
#define GPU_DOORBELL        (*(volatile unsigned long*)(GPU_BASE + 0x00))
#define GPU_STATUS          (*(volatile unsigned long*)(GPU_BASE + 0x08))
#define GPU_KERNEL_ID       (*(volatile unsigned long*)(GPU_BASE + 0x10))
#define GPU_LATENCY_OVR     (*(volatile unsigned long*)(GPU_BASE + 0x18))

#define TESTDEV             (*(volatile unsigned int*)0x100000UL)
#define TESTDEV_PASS        0x5555u

static volatile unsigned long last_kernel_id;

static void launch_kernel(unsigned long latency_cycles) {
    GPU_LATENCY_OVR = latency_cycles;
    GPU_DOORBELL = 0;
    while (GPU_STATUS)
        ;
    last_kernel_id = GPU_KERNEL_ID;
}

void _start(void) {
    launch_kernel(1000);
    TESTDEV = TESTDEV_PASS;
    while (1)
        __asm__ volatile ("wfi");
}
