/* write_burst — tight store loop over a small buffer. */
#include <stdint.h>
#include <stdio.h>

#define N 65536u
static volatile uint8_t buf[N];

int main(void) {
    for (uint32_t i = 0; i < N; i++)
        buf[i] = (uint8_t)(i & 0xFFu);
    printf("write_burst done\n");
    return 0;
}
