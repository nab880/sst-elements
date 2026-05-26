/*
 * riscv_virt_mmio_poke.c — bare-metal MMIO poke for Quetz P0 routing test.
 *
 * Write 3 to mmioEx at 0x80100000, read back (expect 9 = square).
 * Sets virtio testdev at 0x100000 to PASS on success.
 */

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
