/* stride_read — read 1 MiB with compile-time stride (bytes). */
#include <stdint.h>
#include <stdio.h>

#ifndef STRIDE
#define STRIDE 64
#endif

#define SIZE (1024u * 1024u)
static volatile uint8_t buf[SIZE];

int main(void) {
    uint64_t sum = 0;
    for (uint32_t i = 0; i < SIZE; i += STRIDE)
        sum += buf[i];
    printf("stride_read STRIDE=%d sum=%llu\n", STRIDE, (unsigned long long)sum);
    return 0;
}
