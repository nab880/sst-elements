/* coldfire_gpu_async.c — P4 asynchronous-offload vectorAdd on ColdFire (m68k).
 *
 * Same offload as coldfire_gpu.c, but the post-launch cudaThreadSynchronize is
 * posted via the Quetz async aperture (cb_vadd_async): balar holds its deferred
 * response until the kernel finishes while the 32-bit big-endian ColdFire core
 * runs CPU work and polls the completion counter. Result is verified exactly. */

#include "coldfire_uart.h"
#include "coldfire_balar.h"

void kernel_main(void)
{
    uint32_t correct;

    uart_init();
    uart_puts("ColdFire balar vectorAdd ASYNC (NXP mcf5208evb / m68k)\n");
    uart_puts("posting vectorAdd thread-sync to balar GPU...\n");

    correct = cb_vadd_async();

    uart_puts("gpu vectorAdd ASYNC correct=");
    uart_put_u32_dec(correct);
    uart_putc('/');
    uart_put_u32_dec(CB_VEC_N);
    uart_putc('\n');

    testdev_done(correct == CB_VEC_N ? TESTDEV_PASS : TESTDEV_FAIL);
}
