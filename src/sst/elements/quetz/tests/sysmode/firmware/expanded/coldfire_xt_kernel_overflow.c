/* Kernel arg2 ceiling regression. Build once per kernel with OVERFLOW_N set
 * just above that kernel's kMaxKernelInputBytes-derived limit. */

#include <stdint.h>

#include "coldfire_uart.h"
#include "coldfire_devices.h"

#ifndef OVERFLOW_N
#error "OVERFLOW_N must be defined"
#endif

#define WIN 0x71000000UL

void kernel_main(void)
{
    uart_init();
    uart_puts("ColdFire kernel overflow reject (m68k)\n");

    mmio_write32(GPU_ARG0, WIN);
    mmio_write32(GPU_ARG1, WIN + 0x1000UL);
    mmio_write32(GPU_ARG2, (uint32_t)OVERFLOW_N);
    mmio_write32(GPU_ARG3, 0);
    mmio_write32(GPU_DOORBELL, 0);

    uint32_t kernel_id = mmio_read32(GPU_KERNEL_ID);
    uart_puts("overflow reject kernel_id=");
    uart_put_u32_dec(kernel_id);
    uart_putc('\n');
    uart_puts(kernel_id == 0 ? "KERNEL OVERFLOW REJECT PASS\n"
                             : "KERNEL OVERFLOW REJECT FAIL\n");
    testdev_done(kernel_id == 0 ? TESTDEV_PASS : TESTDEV_FAIL);
}
