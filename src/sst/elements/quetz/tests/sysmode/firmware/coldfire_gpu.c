

#include "coldfire_uart.h"
#include "coldfire_balar.h"

void kernel_main(void)
{
    uint32_t correct;

    uart_init();
    uart_puts("ColdFire balar vectorAdd (NXP mcf5208evb / m68k)\n");
    uart_puts("dispatching vectorAdd to balar GPU...\n");

    correct = cb_vadd();

    uart_puts("gpu vectorAdd correct=");
    uart_put_u32_dec(correct);
    uart_putc('/');
    uart_put_u32_dec(CB_VEC_N);
    uart_putc('\n');

    testdev_done(correct == CB_VEC_N ? TESTDEV_PASS : TESTDEV_FAIL);
}
