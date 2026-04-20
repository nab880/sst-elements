/*
 * Ping-pong executable for Vanadis + Hali MMIO coordination.
 * Uses hyades.h for all Vanadis control logic (MMIO and run loop).
 *
 * Build from tests/ with -I.. so hyades.h (in parent carcosa/) is found.
 * The checked-in binary is a ~10KB static-PIE riscv64-linux-musl build:
 *   docker run --rm --platform linux/amd64 --entrypoint sh \
 *     -v "$(pwd)/..:/src" -w /src/tests muslcc/x86_64:riscv64-linux-musl \
 *     -c '/bin/gcc -static -Os -I.. -o pingpong pingpong.c && /bin/strip pingpong'
 */
#include "hyades.h"
#include <unistd.h>

static void ping(void) { write(1, "PING\n", 5); }
static void pong(void) { write(1, "PONG\n", 5); }

int main(int argc, char *argv[]) {
    int role = hyades_role_from_argv(argc, argv);

    hyades_handler_t jump_table[2];
    if (role == 0) {
        jump_table[0] = ping;
        jump_table[1] = pong;
    } else {
        jump_table[0] = pong;
        jump_table[1] = ping;
    }

    hyades_run(jump_table, 2);
    return 0;
}
