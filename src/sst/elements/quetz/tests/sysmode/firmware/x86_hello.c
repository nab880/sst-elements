

#define MB_MAGIC    0x1BADB002u
#define MB_FLAGS    0x00000000u
#define MB_CHECKSUM (-(MB_MAGIC + MB_FLAGS))

__attribute__((section(".multiboot")))
volatile unsigned int multiboot_header[3] = {
    MB_MAGIC, MB_FLAGS, (unsigned int)MB_CHECKSUM
};


#define COM1_THR  0x3F8   
#define COM1_LSR  0x3FD   
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
    outb(COM1_THR + 1, 0x00); 
    outb(COM1_THR + 3, 0x80); 
    outb(COM1_THR + 0, 0x01); 
    outb(COM1_THR + 1, 0x00); 
    outb(COM1_THR + 3, 0x03); 
    outb(COM1_THR + 2, 0xC7); 
}

static void uart_putc(char c) {
    while (!(inb(COM1_LSR) & LSR_THRE));
    outb(COM1_THR, (unsigned char)c);
}

static void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}


void __attribute__((noreturn)) kernel_main(void) {
    uart_init();
    uart_puts("Hello from x86!\n");
    
    outb(0x501, 0x00);
    while (1) __asm__ volatile ("hlt");
}
