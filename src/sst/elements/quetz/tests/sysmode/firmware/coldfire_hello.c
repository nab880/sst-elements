

#include "coldfire_uart.h"


void kernel_main(void)
{
    uart_init();
    uart_puts("Hello from NXP ColdFire (mcf5208evb / m68k)!\n");
    testdev_done(TESTDEV_PASS);
}
