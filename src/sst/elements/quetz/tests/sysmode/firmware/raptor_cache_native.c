/* Public native-RAM diagnostic. Code and volatile data share one 4 KiB page.
 * Control changes deliberately alternate cached and raw reads of the same
 * address on a separate page, retaining its data TLB entry between changes.
 * A separate P1 stack test cleans its live stack before invalidating.
 * This does not modify executable bytes or claim instruction-cache coherence.
 */
#include <stdint.h>
#define R8(a) (*(volatile uint8_t *)(uintptr_t)(a))
#define R32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define CACR_ON 0x94000000u /* DEC, DDPI, default inhibited; ACR0 enables P1. */
#define INITIAL 0x11223344u
#define DIRTY 0x55667788u
#define RAW_NEW 0x99aabbccu
#define STACK_WORDS 48u
#define PROBE 0x80009000u

volatile uint32_t hot_word __attribute__((section(".data.hot"))) = 0;
volatile uint32_t stack_address, stack_before, stack_after, stack_words;

__asm__(".section .text.start,\"ax\"\n.global _start\n_start:\n"
        "move.w #0x2700,%sr\nmove.l #0x0700f000,%sp\n"
        "jsr diagnostic\n1: bra 1b\n"
        ".text\n.global on_cached_stack\non_cached_stack:\n"
        "move.l %a2,-(%sp)\nmove.l %sp,%a2\n"
        "move.l #0x8000fc00,%sp\njsr cached_stack_worker\n"
        "move.l %a2,%sp\nmove.l (%sp)+,%a2\nrts\n"
        /* No stack writes occur between the last clean and DCINVA. The JSR
         * return address and the caller's live frame are part of the sweep. */
        ".global clean_invalidate\nclean_invalidate:\n"
        "move.l #0x94000000,%d0\nmovec %d0,%cacr\nmoveq #0,%d0\n"
        "1: move.l %d0,%a0\ncpushl %dc,(%a0)\naddq.l #1,%d0\n"
        "move.l %d0,%d1\nandi.l #3,%d1\nbne 1b\n"
        "addi.l #12,%d0\ncmpi.l #0x2000,%d0\nbne 1b\n"
        "move.l #0x95000000,%d0\nmovec %d0,%cacr\nrts\n"
        /* Five tags compete for set 257. No intervening data accesses can
         * affect that set before cacheability is disabled for raw inspection. */
        ".global dirty_conflict_writes\ndirty_conflict_writes:\n"
        "move.l #0x80007010,%a0\nmove.l #0x10203040,%d0\nmoveq #4,%d1\n"
        "1: move.l %d0,(%a0)\nadda.l #0x2000,%a0\n"
        "addi.l #0x01010101,%d0\nsubq.l #1,%d1\nbpl 1b\n"
        "moveq #0,%d0\nmovec %d0,%cacr\nrts\n");

extern uint32_t on_cached_stack(void);
extern void clean_invalidate(void);
extern void dirty_conflict_writes(void);
static __attribute__((always_inline)) inline void cacr(uint32_t value) {
    __asm__ __volatile__("movec %0,%%cacr" : : "d"(value) : "memory");
}
static __attribute__((always_inline)) inline void acr(uint32_t value) {
    __asm__ __volatile__("movec %0,%%acr0" : : "d"(value) : "memory");
}
__attribute__((noinline, section(".text.hot"))) uint32_t hot_read(void) {
    return hot_word;
}
__attribute__((noinline, section(".text.hot"))) uint32_t hot_write(uint32_t value) {
    hot_word = value;
    return hot_word;
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
    emit("Raptor Native Cache: "); emit(name); emit("="); emit(word); emit("\n");
}
static void finish(uint32_t value) {
    R32(0x0700fff0u) = value;
    for (;;) __asm__ __volatile__("stop #0x2700" : : : "memory");
}
static void checked(const char *name, uint32_t actual, uint32_t expected) {
    observation(name, actual);
    if (actual != expected) {
        observation("failure", expected);
        finish(0x3333u);
    }
}
static uint32_t sample(unsigned n) { return 0x13570000u + n * 0x01020304u; }

static void dirty_eviction_diagnostic(void) {
    uint32_t raw_words = 0, raw_mask = 0, raw_sum = 0, unexpected = 0;
    uint32_t clean_words = 0, clean_sum = 0, expected_sum = 0;
    cacr(0x01000000u);
    for (unsigned n = 0; n < 5; ++n) R32(0x80007010u + n * 0x2000u) = 0;
    acr(0x8000c020u); cacr(CACR_ON);
    dirty_conflict_writes(); /* Returns with caching disabled. */
    for (unsigned n = 0; n < 5; ++n) {
        uint32_t value = R32(0x80007010u + n * 0x2000u);
        uint32_t expected = 0x10203040u + n * 0x01010101u;
        raw_words += value == expected;
        if (value == expected) raw_mask |= 1u << n;
        unexpected += value != 0 && value != expected;
        raw_sum += value;
        expected_sum += expected;
    }
    checked("eviction_unclean_words", raw_words, 1);
    checked("eviction_unexpected_words", unexpected, 0);
    observation("eviction_raw_mask", raw_mask);
    observation("eviction_unclean_checksum", raw_sum);
    cacr(CACR_ON);
    clean_invalidate();
    cacr(0);
    for (unsigned n = 0; n < 5; ++n) {
        uint32_t value = R32(0x80007010u + n * 0x2000u);
        clean_words += value == 0x10203040u + n * 0x01010101u;
        clean_sum += value;
    }
    checked("eviction_clean_words", clean_words, 5);
    checked("eviction_clean_checksum", clean_sum, expected_sum);
}

uint32_t cached_stack_worker(void) {
    volatile uint32_t words[STACK_WORDS];
    uint32_t before = 0, after = 0, correct = 0;
    stack_address = (uint32_t)(uintptr_t)&words[0];
    for (unsigned n = 0; n < STACK_WORDS; ++n) {
        words[n] = sample(n);
        before += words[n];
    }
    stack_before = before;
    clean_invalidate();
    for (unsigned n = 0; n < STACK_WORDS; ++n) {
        uint32_t value = words[n];
        after += value;
        correct += value == sample(n);
    }
    stack_after = after;
    stack_words = correct;
    return after;
}

void diagnostic(void) {
    uint32_t expected_sum = 0;
    R8(0xfc064008u) = 4;
    cacr(0x01000000u);
    checked("initial_raw", hot_write(INITIAL), INITIAL);
    R32(PROBE) = INITIAL;
    acr(0x8000c020u); cacr(CACR_ON);
    checked("cached_write", hot_write(DIRTY), DIRTY);
    R32(PROBE) = DIRTY;
    uint32_t warm = 0;
    for (unsigned n = 0; n < 32; ++n) warm += hot_read() == DIRTY;
    checked("same_page_reads", warm, 32);
    warm = 0;
    for (unsigned n = 0; n < 32; ++n) warm += R32(PROBE) == DIRTY;
    checked("transition_page_reads", warm, 32);
    acr(0x8000c040u);
    checked("acr_inhibited", R32(PROBE), INITIAL);
    acr(0x8000c020u);
    checked("acr_reenabled", R32(PROBE), DIRTY);
    cacr(0);
    checked("cacr_disabled", R32(PROBE), INITIAL);
    cacr(CACR_ON);
    checked("cacr_reenabled", R32(PROBE), DIRTY);

    /* With AMM clear this matches the coarse 0x80 address byte. With bit 10
     * set it describes 0x80100000..0x801fffff, which excludes native P1 RAM. */
    acr(0x8010c020u);
    checked("amm_clear_coarse_match", R32(PROBE), DIRTY);
    acr(0x8010c420u);
    checked("amm_set_miss", R32(PROBE), INITIAL);
    R32(PROBE) = RAW_NEW;
    checked("amm_raw_write", R32(PROBE), RAW_NEW);
    acr(0x8000c420u);
    checked("amm_exact_match", R32(PROBE), DIRTY);
    acr(0x8011c420u);
    checked("amm_mask_match", R32(PROBE), DIRTY);
    acr(0x400fc420u);
    checked("amm_foreign_byte_miss", R32(PROBE), RAW_NEW);
    acr(0x8000c420u);
    cacr(CACR_ON | 0x01000000u); /* Deliberately discard the old dirty word. */
    checked("amm_after_invalidate", R32(PROBE), RAW_NEW);

    dirty_eviction_diagnostic();
    acr(0x8000c020u); cacr(CACR_ON);
    for (unsigned n = 0; n < STACK_WORDS; ++n) expected_sum += sample(n);
    checked("stack_return", on_cached_stack(), expected_sum);
    checked("stack_before", stack_before, expected_sum);
    checked("stack_after", stack_after, expected_sum);
    checked("stack_words", stack_words, STACK_WORDS);
    observation("hot_code_address", (uint32_t)(uintptr_t)&hot_read);
    observation("hot_data_address", (uint32_t)(uintptr_t)&hot_word);
    observation("transition_data_address", PROBE);
    observation("stack_address", stack_address);
    emit("Raptor Native Cache: result=PASS\n");
    finish(0x5555u);
}
