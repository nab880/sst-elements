/* pointer_chase — linked-list walk to stress cache misses. */
#include <stdint.h>
#include <stdio.h>

#define N 4096u
static uint32_t next_idx[N];
static uint8_t    nodes[N][64];

int main(void) {
    for (uint32_t i = 0; i < N; i++)
        next_idx[i] = (i * 2654435761u) % N;

    uint32_t idx = 0;
    uint64_t sum = 0;
    for (uint32_t k = 0; k < N * 4; k++) {
        sum += nodes[idx][0];
        idx = next_idx[idx];
    }
    printf("pointer_chase sum=%llu\n", (unsigned long long)sum);
    return 0;
}
