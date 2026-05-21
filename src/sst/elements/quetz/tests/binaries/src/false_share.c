/* false_share — two pthreads hammer adjacent lines (OpenMP). */
#include <omp.h>
#include <stdint.h>
#include <stdio.h>

#define N 2
static volatile uint64_t counters[N][8];

int main(void) {
    omp_set_num_threads(2);
#pragma omp parallel for
    for (int t = 0; t < 2; t++) {
        for (int k = 0; k < 10000; k++)
            counters[t][0]++;
    }
    printf("false_share c0=%llu c1=%llu\n",
           (unsigned long long)counters[0][0],
           (unsigned long long)counters[1][0]);
    return 0;
}
