/*
 * fourstate - 4-kernel RISC-V demo for FourStateAgent.
 *
 * Each iteration the agent sends one command index in the sequence
 * 0, 1, 2, 3, 0, 1, 2, 3, ... via MMIO; the CPU runs the matching handler
 * from the jump table. After max_iterations the agent sends -1, which
 * hyades_run() treats as an exit sentinel.
 *
 * Each kernel exercises a different memory-access pattern against a
 * kernel-local static working set, then emits a deterministic checksum
 * over that work. The data path is:
 *
 *     CPU lsq -> Hali(highlink) -> Hali(lowlink) -> dTLB -> L1D
 *
 * so every load/store made by these kernels transits Hali's lowlink, which
 * is where testFourStateRegistryGated.py attaches its PortModuleStateGate.
 * When the gate's predicate matches (by default: currentKernel==DECODE,
 * i.e. the agent-side FSM stage the handler runs in) the gate flips bytes
 * in MemEvent payloads on the wire, which shows up in these checksums as
 * non-reproducible `v=` values for the flipped kernel.
 *
 * Running the same test twice (once with and once without the gate) and
 * diffing stdout-100 / stdout-101 is the cleanest way to see the injection
 * firing: ungated runs produce the *same* `v=` sequence every time, gated
 * runs diverge starting from the first matched kernel call.
 *
 * Build (from carcosa/tests so hyades.h is found via -I..):
 *   riscv64-unknown-linux-gnu-gcc -static -I.. -o fourstate fourstate.c
 * Or with the docker helper pattern used for pingpong:
 *   docker run --rm -v "$(pwd)/..:/src" -w /src/tests ubuntu:22.04 bash -c \
 *     'apt-get update -qq && apt-get install -y -qq gcc-riscv64-linux-gnu && \
 *      riscv64-linux-gnu-gcc -static -I.. -o fourstate fourstate.c'
 *
 * Run (from carcosa/tests, after building fourstate):
 *   sst testFourStateRegistry.py         # no fault injection
 *   sst testFourStateRegistryGated.py    # PortModuleStateGate on lowlink
 * Or point VANADIS_EXE at an absolute binary path.
 */
#include "hyades.h"
#include <unistd.h>

/* Working-set size per kernel. Sized to span multiple 64B cache lines so
 * each kernel exercises a handful of fills/writebacks through Hali's
 * lowlink (not a single hot line). Keep it small so Vanadis sim time
 * stays reasonable. */
#define BUF_LEN 64

/* Per-kernel working sets, one per access pattern. 'volatile' prevents the
 * compiler from folding the loops away; we want the loads/stores to
 * actually hit the data cache (and therefore Hali's lowlink, where a
 * PortModuleStateGate can observe them). Placed in BSS so the addresses
 * are stable across runs. */
static volatile unsigned char  K0_buf[BUF_LEN];        /* K0: byte reads */
static volatile unsigned int   K1_tab[BUF_LEN];        /* K1: strided reads */
static volatile unsigned int   K2_src[BUF_LEN];        /* K2: read side */
static volatile unsigned int   K2_dst[BUF_LEN];        /* K2: write side */
static volatile unsigned int   K3_sink[BUF_LEN];       /* K3: writes */

/* Monotonic call counter, incremented at the end of kernel3 (i.e. once per
 * full iteration across all 4 kernels). Used by K2/K3 to vary their
 * written values, so each run produces a distinct but deterministic
 * checksum sequence that corruption can perturb. */
static unsigned int k_iter = 0;

static const char *role_tag = "r?";

/* Minimal "unsigned -> 8 hex chars + newline" emitter. Avoids printf (and
 * its libc-static-init cost) and any dynamic allocation. */
static void write_hex8(unsigned int v) {
    char buf[9];
    for (int i = 7; i >= 0; i--) {
        unsigned int nib = v & 0xF;
        buf[i] = (nib < 10) ? (char)('0' + nib) : (char)('a' + nib - 10);
        v >>= 4;
    }
    buf[8] = '\n';
    write(1, buf, 9);
}

/* Emit "<tag3><role_tag> v=<hex>\n" — e.g. "K2 r0 v=deadbeef\n". */
static void emit(const char *tag3, unsigned int v) {
    write(1, tag3,     3);
    write(1, role_tag, 2);
    write(1, " v=",    3);
    write_hex8(v);
}

/* K0 — sequential byte reads.
 * Pattern: load every byte of K0_buf once, accumulate into a 32-bit sum.
 * Exercises: dense stream of contiguous 8-bit loads; high spatial locality
 * (the whole array fits in a single L1 line run). */
static void kernel0(void) {
    unsigned int sum = 0;
    for (int i = 0; i < BUF_LEN; i++) sum += K0_buf[i];
    emit("K0 ", sum);
}

/* K1 — strided word reads.
 * Pattern: xor-reduce every 4th word of K1_tab. With 4B words and 64B
 * cache lines this touches one word per line, so each iteration is a
 * cold-line fill (or near it) — useful for exercising lowlink response
 * traffic. */
static void kernel1(void) {
    unsigned int xorv = 0;
    for (int i = 0; i < BUF_LEN; i += 4) xorv ^= K1_tab[i];
    emit("K1 ", xorv);
}

/* K2 — read-modify-write.
 * Pattern: load K2_src[i], combine with k_iter, store into K2_dst[i].
 * Exercises: interleaved load/store pairs producing both read-response
 * and write-request MemEvents on the lowlink. The checksum (xor of the
 * first and last written element) depends on every store completing
 * correctly, so flipped stores show up immediately. */
static void kernel2(void) {
    for (int i = 0; i < BUF_LEN; i++) {
        K2_dst[i] = K2_src[i] + k_iter + (unsigned int)i;
    }
    emit("K2 ", K2_dst[0] ^ K2_dst[BUF_LEN - 1]);
}

/* K3 — sequential word writes, then read-back checksum.
 * Pattern: fill K3_sink with a per-iteration pattern, then sum it back.
 * Exercises: store-heavy burst followed by reads against the just-written
 * lines. A flip on a write-path or a read-response surfaces in the sum. */
static void kernel3(void) {
    unsigned int pat = k_iter * 0x9E3779B1u;   /* fractional golden ratio */
    for (int i = 0; i < BUF_LEN; i++) {
        K3_sink[i] = pat + (unsigned int)i;
    }
    unsigned int sum = 0;
    for (int i = 0; i < BUF_LEN; i++) sum += K3_sink[i];
    emit("K3 ", sum);
    k_iter++;
}

/* Initialize the read-side working sets with non-zero, address-dependent
 * values. Done once at startup so subsequent K0/K1/K2 reads see stable
 * deterministic data (absent fault injection). */
static void init_data(void) {
    for (int i = 0; i < BUF_LEN; i++) {
        K0_buf[i]  = (unsigned char)(i * 3u + 7u);
        K1_tab[i]  = (unsigned int)i * 0x01010101u + 0x13579BDFu;
        K2_src[i]  = (unsigned int)i * 0xDEADBEEFu;
        K2_dst[i]  = 0;
        K3_sink[i] = 0;
    }
}

int main(int argc, char *argv[]) {
    int role = hyades_role_from_argv(argc, argv);
    role_tag = (role == 0) ? "r0" : "r1";

    init_data();

    hyades_handler_t jump_table[4];
    jump_table[0] = kernel0;
    jump_table[1] = kernel1;
    jump_table[2] = kernel2;
    jump_table[3] = kernel3;

    hyades_run(jump_table, 4);
    return 0;
}
