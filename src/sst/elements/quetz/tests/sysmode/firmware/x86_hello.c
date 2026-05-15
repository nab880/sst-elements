/*
 * x86_hello.c — multiboot bare-metal hello for QEMU x86_64 (q35 machine).
 *
 * Loaded by QEMU using -kernel (QEMU implements a built-in multiboot loader).
 * Starts in 32-bit protected mode with flat segments.
 *
 * UART:  NS16550 COM1 at I/O port 0x3F8 (standard x86 BIOS COM port).
 *        x86 UART is I/O-port-mapped, so the TCG plugin does NOT capture
 *        these writes.  Instead, the test harness captures UART output by
 *        passing -serial file:<path> in qemu_args and verifying the file.
 * Exit:  QEMU isa-debug-exit device at I/O port 0x501 (added via qemu_args).
 *        Write any byte to port 0x501 to exit.
 *
 * Build:
 *   gcc -m32 -ffreestanding -fno-stack-protector -nostdlib -nostartfiles \
 *       -O2 -T link_x86.ld x86_hello.c -o x86_hello
 */

/* Multiboot header — must be in the first 8 KiB of the kernel image */
#define MB_MAGIC    0x1BADB002u
#define MB_FLAGS    0x00000000u
#define MB_CHECKSUM (-(MB_MAGIC + MB_FLAGS))

__attribute__((section(".multiboot")))
volatile unsigned int multiboot_header[3] = {
    MB_MAGIC, MB_FLAGS, (unsigned int)MB_CHECKSUM
};

/* ---- COM1 NS16550 via x86 I/O ports ---- */
#define COM1_THR  0x3F8   /* Transmit Holding Register (DLAB=0) */
#define COM1_LSR  0x3FD   /* Line Status Register */
#define LSR_THRE  (1u << 5)

static inline void outb(unsigned short port, unsigned char val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline unsigned char inb(unsigned short port) {
    unsigned char v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void uart_init(void) {
    outb(COM1_THR + 1, 0x00); /* disable interrupts */
    outb(COM1_THR + 3, 0x80); /* DLAB=1 */
    outb(COM1_THR + 0, 0x01); /* divisor low: 115200 baud @ 1.8MHz */
    outb(COM1_THR + 1, 0x00); /* divisor high */
    outb(COM1_THR + 3, 0x03); /* 8N1, DLAB=0 */
    outb(COM1_THR + 2, 0xC7); /* FIFO on */
}

static void uart_putc(char c) {
    while (!(inb(COM1_LSR) & LSR_THRE));
    outb(COM1_THR, (unsigned char)c);
}

static void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}

/* ---- Entry point (called by QEMU's multiboot stub) ---- */
void __attribute__((noreturn)) kernel_main(void) {
    uart_init();
    uart_puts("Hello from x86!\n");
    /* Exit via isa-debug-exit device at port 0x501 (write 0 → QEMU exits) */
    outb(0x501, 0x00);
    while (1) __asm__ volatile ("hlt");
}
