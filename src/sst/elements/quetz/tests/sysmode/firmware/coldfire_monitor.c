

#include "coldfire_uart.h"
#include "coldfire_balar.h"


static uint32_t g_data[8] = {
    0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u,
    0x55555555u, 0x66666666u, 0x77777777u, 0x88888888u
};

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}


static char *next_token(char **p)
{
    char *s = *p;
    while (*s == ' ' || *s == '\t')
        s++;
    if (!*s) {
        *p = s;
        return 0;
    }
    char *start = s;
    while (*s && *s != ' ' && *s != '\t')
        s++;
    if (*s) {
        *s = '\0';
        s++;
    }
    *p = s;
    return start;
}

static uint32_t parse_hex(const char *s)
{
    uint32_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    for (;;) {
        char c = *s++;
        uint32_t d;
        if (c >= '0' && c <= '9')      d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else break;
        v = (v << 4) | d;
    }
    return v;
}


static int read_line(char *buf, int cap)
{
    int n = 0;
    for (;;) {
        char c = uart_getc();
        if (c == '\r' || c == '\n') {
            uart_putc('\n');
            break;
        }
        if ((c == 8 || c == 127) && n > 0) {   
            n--;
            uart_puts("\b \b");
            continue;
        }
        if (n < cap - 1) {
            buf[n++] = c;
            uart_putc(c);                       
        }
    }
    buf[n] = '\0';
    return n;
}

static void cmd_help(void)
{
    uart_puts("commands: help | peek <a> | poke <a> <v> | "
              "dump <a> [n] | run | gpu | quit\n");
}


static void cmd_gpu(void)
{
    uint32_t correct;
    uart_puts("dispatching vectorAdd to balar GPU...\n");
    correct = cb_vadd();
    uart_puts("gpu vectorAdd correct=");
    uart_put_u32_dec(correct);
    uart_putc('/');
    uart_put_u32_dec(CB_VEC_N);
    uart_putc('\n');
}

static void cmd_peek(char **p)
{
    char *a = next_token(p);
    if (!a) {
        uart_puts("usage: peek <addr>\n");
        return;
    }
    uint32_t addr = parse_hex(a);
    uart_put_u32_hex(addr);
    uart_puts(": ");
    uart_put_u32_hex(*(volatile uint32_t*)addr);
    uart_putc('\n');
}

static void cmd_poke(char **p)
{
    char *a = next_token(p);
    char *v = next_token(p);
    if (!a || !v) {
        uart_puts("usage: poke <addr> <val>\n");
        return;
    }
    *(volatile uint32_t*)parse_hex(a) = parse_hex(v);
    uart_puts("ok\n");
}

static void cmd_dump(char **p)
{
    char *a = next_token(p);
    char *n = next_token(p);
    if (!a) {
        uart_puts("usage: dump <addr> [n]\n");
        return;
    }
    uint32_t addr = parse_hex(a);
    uint32_t cnt  = n ? parse_hex(n) : 4u;
    for (uint32_t i = 0; i < cnt; i++) {
        uart_put_u32_hex(addr + i * 4u);
        uart_puts(": ");
        uart_put_u32_hex(*(volatile uint32_t*)(addr + i * 4u));
        uart_putc('\n');
    }
}

static void cmd_run(void)
{
    volatile uint32_t *d = g_data;
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++)
        sum += d[i];
    uart_puts("checksum=");
    uart_put_u32_hex(sum);
    uart_putc('\n');
}

void kernel_main(void)
{
    char line[80];

    uart_init();
    uart_puts("ColdFire dBUG-style monitor (NXP mcf5208evb / m68k)\n");
    uart_puts("type 'help' for commands\n");

    for (;;) {
        uart_puts("dbug> ");
        read_line(line, (int)sizeof(line));

        char *p   = line;
        char *cmd = next_token(&p);
        if (!cmd)
            continue;

        if (str_eq(cmd, "help"))      cmd_help();
        else if (str_eq(cmd, "peek")) cmd_peek(&p);
        else if (str_eq(cmd, "poke")) cmd_poke(&p);
        else if (str_eq(cmd, "dump")) cmd_dump(&p);
        else if (str_eq(cmd, "run"))  cmd_run();
        else if (str_eq(cmd, "gpu"))  cmd_gpu();
        else if (str_eq(cmd, "quit")) {
            uart_puts("bye\n");
            break;
        } else {
            uart_puts("unknown: ");
            uart_puts(cmd);
            uart_putc('\n');
        }
    }

    testdev_done(TESTDEV_PASS);
}
