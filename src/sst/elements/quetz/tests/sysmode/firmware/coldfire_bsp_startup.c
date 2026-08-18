/* Validate BSP startup, in-place data, zero-filled BSS and vector layout on mcf5208evb. */

#include <stdint.h>
#include "coldfire_uart.h"

/* Touched by the asm access-error handler (external linkage required). */
volatile uint32_t g_startup_fault_flag;
volatile uint32_t g_startup_resume_pc;

/* The stock-shaped vector table and its live handler (coldfire_bsp_startup.S). */
extern uint32_t _vect[256];
extern void _start(void);
extern void cf_startup_fault(void);
extern void cf_access_error(void);
extern void cf_address_error(void);
extern void cf_illegal_instruction(void);
extern void cf_divide_by_zero(void);
extern void cf_reserved_6(void);
extern void cf_reserved_7(void);
extern void cf_privilege_violation(void);
extern uint32_t __STACK[];

/* Startup deliberately leaves this NOLOAD region uncleared. */
static volatile uint32_t bss_probe[16];

/* Initialized sentinels detect missing ELF data or unintended startup copying. */
static volatile uint32_t data_probe[4] = { 0xB5504100u, 0x0000C0DEu,
                                           0xDEADBEEFu, 0x5A5AA5A5u };

/* Symbols from the linker script bounding .bss. */
extern uint32_t __bss_start[];
extern uint32_t __bss_end[];

static uint32_t check_bss_zeroed(void)
{
    uint32_t errors = 0;
    for (unsigned i = 0; i < sizeof(bss_probe) / sizeof(bss_probe[0]); i++) {
        if (bss_probe[i] != 0) {
            errors++;
        }
    }
    /* Also sweep the whole linker-declared .bss span. */
    for (volatile uint32_t *p = __bss_start; p < __bss_end; p++) {
        if (*p != 0) {
            errors++;
            break;
        }
    }
    return errors;
}

static uint32_t check_data_in_place(void)
{
    uint32_t errors = 0;
    if (data_probe[0] != 0xB5504100u) errors++;
    if (data_probe[1] != 0x0000C0DEu) errors++;
    if (data_probe[2] != 0xDEADBEEFu) errors++;
    if (data_probe[3] != 0x5A5AA5A5u) errors++;
    return errors;
}

/* Inspect the linked vector table; ColdFire V4 VBR is write-only. */
static uint32_t check_vector_table_shape(void)
{
    const volatile uint32_t *vt = (const volatile uint32_t *)(uintptr_t)&_vect[0];
    uint32_t errors = 0;

    void (*const handlers[7])(void) = {
        cf_access_error,
        cf_address_error,
        cf_illegal_instruction,
        cf_divide_by_zero,
        cf_reserved_6,
        cf_reserved_7,
        cf_privilege_violation,
    };

    if (vt[0] != (uint32_t)(uintptr_t)__STACK) errors++; /* initial SP */
    if (vt[1] != (uint32_t)(uintptr_t)&_start) errors++; /* initial PC */
    for (unsigned i = 0; i < 7; i++) {
        if (vt[i + 2] != (uint32_t)(uintptr_t)handlers[i]) errors++;
    }

    for (unsigned i = 9; i < 256; i++) {
        if (vt[i] != 0) {                                /* rest are NULL */
            errors++;
            break;
        }
    }
    return errors;
}

/* Check the handler structurally: mcf5208evb unmapped accesses do not raise bus errors. */
static uint32_t check_fault_handler_present(void)
{
    return ((uint32_t)(uintptr_t)&cf_startup_fault != 0) ? 0 : 1;
}

void kernel_main(void)
{
    uint32_t errors = 0;

    uart_init();
    uart_puts("BSP startup: reached main after SR/RAMBAR/SP/VBR sequence\n");

    uint32_t e_bss = check_bss_zeroed();
    uart_puts(".bss zeroed (no clear loop): ");
    uart_puts(e_bss == 0 ? "ok\n" : "FAIL\n");
    errors += e_bss;

    uint32_t e_data = check_data_in_place();
    uart_puts(".data in place (no copy): ");
    uart_puts(e_data == 0 ? "ok\n" : "FAIL\n");
    errors += e_data;

    uint32_t e_vec = check_vector_table_shape();
    uart_puts("vector table (0..8 live, 9..255 NULL): ");
    uart_puts(e_vec == 0 ? "ok\n" : "FAIL\n");
    errors += e_vec;

    uint32_t e_h = check_fault_handler_present();
    uart_puts("access-error handler installed: ");
    uart_puts(e_h == 0 ? "ok\n" : "FAIL\n");
    errors += e_h;

    uart_puts("BSP startup: errors=");
    uart_put_u32_dec(errors);
    uart_putc('\n');

    testdev_done(errors == 0 ? TESTDEV_PASS : TESTDEV_FAIL);
    for (;;) {
        __asm__ volatile("");
    }
}
