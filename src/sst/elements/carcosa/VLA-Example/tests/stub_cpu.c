/* Phase 2 CPU: MMIO only; SST delay agent supplies kernel time. */
#include "hyades.h"

static void noop(void) { (void)0; }

int main(void)
{
    hyades_handler_t table[18];
    for (int i = 0; i < 18; ++i)
        table[i] = noop;
    hyades_run(table, 18);
    return 0;
}
