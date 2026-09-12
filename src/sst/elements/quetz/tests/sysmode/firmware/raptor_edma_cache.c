/* Public diagnostic: native-RAM copyback data versus real eDMA backing access.
 * The CPU's stack is uncached SRAM; input/output are P1 RAM. No IRQ route is
 * invented: one software-requested minor loop is polled through DONE and ERR.
 */
#include <stdint.h>
#ifndef DO_CLEAN
#define DO_CLEAN 1
#endif
#ifndef DO_INVALIDATE
#define DO_INVALIDATE 1
#endif
#define R8(a) (*(volatile uint8_t *)(uintptr_t)(a))
#define R16(a) (*(volatile uint16_t *)(uintptr_t)(a))
#define R32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define INPUT 0x80009000u
#define OUTPUT 0x80009040u
#define DMA 0xfc044000u
#define TCD (DMA + 0x1000u)
#define SENTINEL 0xa5a5a5a5u
#define CACR_ON 0x84000000u
__asm__(".text\n.global _start\n_start:\nmove.w #0x2700,%sr\n"
        "move.l #0x0700f000,%sp\njsr diagnostic\n1: bra 1b\n");
static void cacr(uint32_t value) {
    __asm__ __volatile__("movec %0,%%cacr" : : "d"(value) : "memory");
}
static void acr(uint32_t value) {
    __asm__ __volatile__("movec %0,%%acr0" : : "d"(value) : "memory");
}
static void emit(const char *text) {
    while (*text) {
        while (!(R8(0xfc064004u) & 4u)) {}
        R8(0xfc06400cu) = (uint8_t)*text++;
    }
}
static void observation(const char *name, uint32_t value) {
    const char digits[] = "0123456789abcdef";
    char word[9];
    for (unsigned n = 0; n < 8; ++n) word[n] = digits[(value >> (28u - 4u*n)) & 15u];
    word[8] = 0;
    emit("Raptor eDMA Cache: "); emit(name); emit("="); emit(word); emit("\n");
}
static void finish(uint32_t value) {
    R32(0x0700fff0u) = value;
    for (;;) __asm__ __volatile__("stop #0x2700" : : : "memory");
}
static void fail(uint32_t code) { observation("failure", code); finish(0x3333u); }
static void check(uint32_t actual, uint32_t expected, uint32_t code) {
    if (actual != expected) fail(code);
}
static uint32_t sample(unsigned n) { return 0x10203040u + n * 0x01010101u; }
static void clean_input(void) {
    for (uint32_t line = INPUT; line < INPUT + 64; line += 16) {
        for (uint32_t way = 0; way < 4; ++way) {
            uint32_t operand = (line & 0x1ff0u) | way;
            __asm__ __volatile__("move.l %0,%%a0\ncpushl %%dc,(%%a0)"
                                 : : "d"(operand) : "a0", "memory");
        }
    }
}
void diagnostic(void) {
    uint32_t primed = 0, stale = 0, correct = 0, cpu_sum = 0, raw_sum = 0;
    R8(0xfc064008u) = 4;
    cacr(0x01000000u);
    for (unsigned n = 0; n < 16; ++n) {
        R32(INPUT + 4*n) = 0; R32(OUTPUT + 4*n) = SENTINEL;
    }
    acr(0x8000c020u); cacr(CACR_ON);
    for (unsigned n = 0; n < 16; ++n) {
        R32(INPUT + 4*n) = sample(n);
        primed += R32(OUTPUT + 4*n) == SENTINEL;
    }
    uint32_t cached_input = R32(INPUT);
    check(cached_input, sample(0), 0x4001);
    check(primed, 16, 0x4002);
#if DO_CLEAN
    clean_input();
#endif
    acr(0x8000c040u);
    uint32_t backing_input = R32(INPUT);
    check(backing_input, DO_CLEAN ? sample(0) : 0, 0x4003);
    acr(0x8000c020u);
    R32(TCD) = INPUT; R16(TCD + 4) = 0x0202; R16(TCD + 6) = 4;
    R32(TCD + 8) = 64; R32(TCD + 12) = (uint32_t)-64;
    R32(TCD + 16) = OUTPUT; R16(TCD + 20) = 1; R16(TCD + 22) = 4;
    R32(TCD + 24) = (uint32_t)-64; R16(TCD + 28) = 1; R16(TCD + 30) = 8;
    R8(DMA + 0x1e) = 0;
    uint32_t done = 0;
    for (unsigned spin = 0; spin < 2000000; ++spin) {
        if (R16(DMA + 0x2e)) fail(0x4004);
        done = R16(TCD + 30);
        if (done & 0x80u) break;
    }
    check(done, 0x88, 0x4005);
    check(R16(TCD + 20), 1, 0x4006);
    check(R32(TCD), INPUT, 0x4007);
    check(R32(TCD + 16), OUTPUT, 0x4008);
    for (unsigned n = 0; n < 16; ++n) stale += R32(OUTPUT + 4*n) == SENTINEL;
    check(stale, 16, 0x4009);
#if DO_INVALIDATE
    cacr(CACR_ON | 0x01000000u);
#endif
    for (unsigned n = 0; n < 16; ++n) {
        uint32_t value = R32(OUTPUT + 4*n);
        check(value, DO_INVALIDATE ? (DO_CLEAN ? sample(n) : 0) : SENTINEL, 0x4010 + n);
        correct += value == sample(n); cpu_sum += value;
    }
    acr(0x8000c040u);
    for (unsigned n = 0; n < 16; ++n) {
        uint32_t value = R32(OUTPUT + 4*n);
        check(value, DO_CLEAN ? sample(n) : 0, 0x4030 + n);
        raw_sum += value;
    }
    emit("Raptor eDMA Cache: case=");
#if !DO_CLEAN
    emit("missing-clean");
#elif !DO_INVALIDATE
    emit("missing-invalidate");
#else
    emit("full");
#endif
    emit("\n");
    observation("input_address", INPUT); observation("output_address", OUTPUT);
    observation("cached_input", cached_input); observation("backing_input", backing_input);
    observation("primed_words", primed); observation("done", done);
    observation("error", R16(DMA + 0x2e)); observation("citer", R16(TCD + 20));
    observation("stale_words", stale); observation("correct_words", correct);
    observation("cpu_checksum", cpu_sum); observation("backing_checksum", raw_sum);
    emit("Raptor eDMA Cache: result=PASS\n"); finish(0x5555u);
}
