

#define MMIO_BASE  0x80100000UL
#define MMIO_REG   (*(volatile unsigned int*)MMIO_BASE)

#define TESTDEV      (*(volatile unsigned int*)0x100000UL)
#define TESTDEV_PASS 0x5555u
#define TESTDEV_FAIL 0xdeadbeefu

void _start(void) {
    MMIO_REG = 3;
    unsigned int got = MMIO_REG;
    if (got == 9u)
        TESTDEV = TESTDEV_PASS;
    else
        TESTDEV = TESTDEV_FAIL;
    while (1)
        __asm__ volatile ("wfi");
}
