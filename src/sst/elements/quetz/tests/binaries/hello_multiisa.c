/*
 * hello_multiisa.c — simple hello-world with FP work for ISA classifier tests.
 *
 * Performs a small FP-heavy loop (Leibniz series for pi) so that:
 *   - Integer loads/stores appear (stack, argv access)
 *   - FP loads/stores appear (double array access)
 *   - On x86_64 with SSE2 the FP loads will be 8-byte doubles
 *   - On AArch64 the FP loads will come through NEON/ASIMD registers
 *
 * Compiled as:
 *   x86_64:  gcc    -O2 -static -o hello_x86_64   hello_multiisa.c
 *   aarch64: aarch64-linux-gnu-gcc -O2 -static -o hello_aarch64 hello_multiisa.c
 *   mipsel:  already provided by vanadis tree (hello-world / sqrt-float)
 */

#include <stdio.h>
#include <math.h>

int main(void)
{
    double s = 0.0;
    double sign = 1.0;
    int i;
    for (i = 0; i < 10000; i++) {
        s += sign / (2*i + 1);
        sign = -sign;
    }
    printf("pi ~ %.6f\n", 4.0 * s);
    return 0;
}
