/* Public functional diagnostic: independent CPU caches, shared SST backing.
 * Native SRAM handshakes deliberately bypass the cached data window.
 * CPUSHL uses the explicit 32-KiB / 4-way / 512-set / 16-byte V4e proxy.
 * Reset preserves tags/data and clears controls; startup invalidates explicitly.
 */
#include <stdint.h>
#ifndef CORE_ID
#define CORE_ID 0
#endif
#ifndef DO_CLEAN
#define DO_CLEAN 1
#endif
#ifndef DO_INVALIDATE
#define DO_INVALIDATE 1
#endif
#ifndef TEST_RESET
#define TEST_RESET 0
#endif
#define WORD(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define BYTE(a) (*(volatile uint8_t *)(uintptr_t)(a))
#define HALF(a) (*(volatile uint16_t *)(uintptr_t)(a))
#ifndef DATA_BASE
#define DATA_BASE 0x71000000u
#endif
#ifndef ACR_BASE
#define ACR_BASE 0x71000000u
#endif
#ifndef TEST_NATIVE_RAM
#define TEST_NATIVE_RAM 0
#endif
#define DATA DATA_BASE
#define PEER_PROBE (DATA_BASE + 0x20u)
#define PEER_DIRTY (DATA_BASE + 0x30u)
#define RESET_PROBE1 (DATA_BASE + 0x40u)
#define OWN_DIRTY (DATA_BASE + 0x50u)
#define RESET_PROBE2 (DATA_BASE + 0x60u)
#define OLD 0x11223344u
#define NEW1 0x55667788u
#define NEW2 0xaabbccddu
#define PROBE 0x2468ace0u
#define DIRTY1 0x13579bdfu
#define DIRTY2 0xfedcba98u
#define OWN 0xc001cafeu
#define RESET1 0x1234abcdu
#define RESET2 0x5678ef01u
#define CACR_ON 0x84000000u
#define ACR_COPYBACK (ACR_BASE | 0x0000c020u)
#define ACR_UNCACHED (ACR_BASE | 0x0000c040u)
#define PHASE0 WORD(0x07001000u)
#define PHASE1 WORD(0x07001004u)
#define BOOTS WORD(0x07001008u)
#define FAILURE WORD(0x0700100cu)
#define OBS(n) WORD(0x07001100u + 4u * (n))
enum { CACR_PEER, ACR_PEER, WRITER_AFTER_INVALIDATE, PRIMED, STALE,
       AFTER, WRITER_AFTER_READER, BACKING, PEER_WRITE, READER_DIRTY, READER_AFTER_INVALIDATE,
       READER_BACKING, RESET_DIRTY,
       RESET_INITIAL, RESET_ACR_BYPASS, RESET_RETAINED, RESET_INVALIDATED,
       PRIMARY_RESET1, RESET_CACR_BYPASS, RESET_RETAINED2, RESET_INVALIDATED2,
       PRIMARY_RESET2, OBS_COUNT };

#if TEST_NATIVE_RAM
#if CORE_ID == 0
#define INITIAL_SP "0x0700f000"
#else
#define INITIAL_SP "0x0700e000"
#endif
#elif CORE_ID == 0
#define INITIAL_SP "0x8000fc00"
#else
#define INITIAL_SP "0x4000fc00"
#endif
#if CORE_ID == 0
__asm__(".text\n.global _start\n_start:\nmove.w #0x2700,%sr\n"
        "move.l #" INITIAL_SP ",%sp\njsr diagnostic\n1: bra 1b\n");
#else
__asm__(".text\n.global _start\n_start:\nmove.w #0x2700,%sr\n"
        "move.l #" INITIAL_SP ",%sp\njsr diagnostic\n1: bra 1b\n");
#endif
static void cacr(uint32_t value) {
    __asm__ __volatile__("movec %0,%%cacr" : : "d"(value) : "memory");
}
static void acr(uint32_t value) {
    __asm__ __volatile__("movec %0,%%acr0" : : "d"(value) : "memory");
}
static void invalidate(void) { cacr(CACR_ON | 0x01000000u); }
static void emit(const char *text) {
    while (*text) {
        while (!(BYTE(0xfc064004u) & 4u)) {}
        BYTE(0xfc06400cu) = (uint8_t)*text++;
    }
}
static void hex(uint32_t value) {
    const char digits[] = "0123456789abcdef";
    char text[9];
    for (unsigned n = 0; n < 8; ++n) text[n] = digits[(value >> (28u - 4u*n)) & 15u];
    text[8] = 0; emit(text);
}
static void finish(uint32_t value) {
    WORD(0x0700fff0u + 4u * CORE_ID) = value;
    for (;;) __asm__ __volatile__("stop #0x2700" : : : "memory");
}
static void fail(uint32_t code) {
    FAILURE = code;
#if CORE_ID == 0
    emit("Raptor Multicore Cache: failure="); hex(code); emit("\n");
#endif
    finish(0x3333u);
}
static void check(uint32_t actual, uint32_t expected, uint32_t code) {
    if (actual != expected) fail(code);
}
static void wait_phase(volatile uint32_t *phase, uint32_t value) {
    for (uint32_t spin = 0; spin < 10000000u; ++spin) {
        if (FAILURE) fail(FAILURE);
        if (*phase == value) return;
    }
    fail(0x1000u + value + CORE_ID * 0x100u);
}
static void wait_for_rehold(void) {
    for (;;) {
        if (FAILURE) fail(FAILURE);
        __asm__ __volatile__("nop" : : : "memory");
    }
}
static void clean_data_set(void) {
    /* All four ways for DATA's index, not a virtual-address range. */
    for (uint32_t way = 0; way < 4; ++way) {
        uint32_t operand = (DATA_BASE & 0x1ff0u) | way;
        __asm__ __volatile__("move.l %0,%%a0\ncpushl %%dc,(%%a0)"
                             : : "d"(operand) : "a0", "memory");
    }
}
static void release(void) { HALF(0xfc084010u) = 0x8080u; }
static void rehold(void) { HALF(0xfc084010u) = 0x8000u; }

#if CORE_ID == 0
static void report(void) {
    static const char *const names[] = {
        "cacr_peer", "acr_peer", "writer_after_peer_invalidate", "primed",
        "after_clean_before_invalidate", "after_maintenance", "writer_after_reader",
        "backing_after_maintenance", "peer_uncached_write", "reader_dirty",
        "reader_dirty_after_invalidate", "reader_backing", "reset_dirty_before",
        "reset_initial", "reset_acr_bypass", "reset_retained", "reset_invalidated",
        "primary_survives_reset1", "reset_cacr_bypass", "reset_retained2",
        "reset_invalidated2", "primary_survives_reset2"
    };
    emit("Raptor Multicore Cache: data_address="); hex(DATA_BASE); emit("\n");
    emit("Raptor Multicore Cache: case=");
#if TEST_RESET
    emit("reset");
#elif !DO_CLEAN
    emit("missing-clean");
#elif !DO_INVALIDATE
    emit("missing-invalidate");
#else
    emit("full");
#endif
    emit("\n");
    for (unsigned n = 0; n < (TEST_RESET ? OBS_COUNT : RESET_DIRTY); ++n) {
        emit("Raptor Multicore Cache: "); emit(names[n]); emit("="); hex(OBS(n)); emit("\n");
    }
    emit("Raptor Multicore Cache: result=PASS\n");
}
void diagnostic(void) {
    BYTE(0xfc064008u) = 4;
    PHASE0 = PHASE1 = BOOTS = FAILURE = 0;
    for (unsigned n = 0; n < OBS_COUNT; ++n) OBS(n) = 0;
    cacr(0x01000000u);
    for (unsigned n = 0; n < 256; n += 4) WORD(DATA + n) = 0;
    WORD(DATA) = OLD;
    acr(ACR_COPYBACK); cacr(CACR_ON);
    release();
    wait_phase(&PHASE1, 1);
    WORD(DATA) = NEW1;
    check(WORD(DATA), NEW1, 0x2001);
    PHASE0 = 1;
    wait_phase(&PHASE1, 2);
    WORD(DATA) = NEW2;
    PHASE0 = 2;
    wait_phase(&PHASE1, 3);
    OBS(WRITER_AFTER_INVALIDATE) = WORD(DATA);
    check(OBS(WRITER_AFTER_INVALIDATE), NEW2, 0x2002);
    PHASE0 = 3;
    wait_phase(&PHASE1, 4);
#if DO_CLEAN
    clean_data_set();
#endif
    PHASE0 = 4;
    wait_phase(&PHASE1, 5);
    OBS(WRITER_AFTER_READER) = WORD(DATA);
    check(OBS(WRITER_AFTER_READER), NEW2, 0x2003);
    acr(ACR_UNCACHED);
    OBS(BACKING) = WORD(DATA);
    check(OBS(BACKING), DO_CLEAN ? NEW2 : OLD, 0x2004);
    OBS(PEER_WRITE) = WORD(PEER_PROBE);
    check(OBS(PEER_WRITE), PROBE, 0x2005);
    OBS(READER_BACKING) = WORD(PEER_DIRTY);
    check(OBS(READER_BACKING), 0, 0x2006);
    PHASE0 = 5;
#if TEST_RESET
    wait_phase(&PHASE1, 6);
    acr(ACR_COPYBACK); WORD(OWN_DIRTY) = OWN;
    rehold(); PHASE0 = 6; release();
    wait_phase(&PHASE1, 7);
    acr(ACR_UNCACHED); OBS(RESET_ACR_BYPASS) = WORD(RESET_PROBE1);
    check(OBS(RESET_ACR_BYPASS), RESET1, 0x2011);
    acr(ACR_COPYBACK); OBS(PRIMARY_RESET1) = WORD(OWN_DIRTY);
    check(OBS(PRIMARY_RESET1), OWN, 0x2012);
    PHASE0 = 7;
    wait_phase(&PHASE1, 8);
    rehold(); PHASE0 = 8; release();
    wait_phase(&PHASE1, 9);
    acr(ACR_UNCACHED); OBS(RESET_CACR_BYPASS) = WORD(RESET_PROBE2);
    check(OBS(RESET_CACR_BYPASS), RESET2, 0x2013);
    acr(ACR_COPYBACK); OBS(PRIMARY_RESET2) = WORD(OWN_DIRTY);
    check(OBS(PRIMARY_RESET2), OWN, 0x2014);
    PHASE0 = 9;
    wait_phase(&PHASE1, 10);
#else
    wait_phase(&PHASE1, 6);
#endif
    report(); finish(0x5555u);
}
#else
void diagnostic(void) {
    uint32_t boot = BOOTS + 1;
    BOOTS = boot;
#if TEST_RESET
    if (boot == 2) {
        OBS(RESET_INITIAL) = WORD(PEER_DIRTY);
        check(OBS(RESET_INITIAL), 0, 0x3011);
        /* Enable CACR alone: reset ACR0 must still leave default uncached. */
        cacr(CACR_ON); WORD(RESET_PROBE1) = RESET1; PHASE1 = 7;
        wait_phase(&PHASE0, 7);
        acr(ACR_COPYBACK); OBS(RESET_RETAINED) = WORD(PEER_DIRTY);
        check(OBS(RESET_RETAINED), DIRTY1, 0x3012);
        invalidate(); OBS(RESET_INVALIDATED) = WORD(PEER_DIRTY);
        check(OBS(RESET_INVALIDATED), 0, 0x3013);
        WORD(PEER_DIRTY) = DIRTY2; PHASE1 = 8;
        wait_for_rehold();
    }
    if (boot == 3) {
        /* Set ACR alone: reset CACR must keep the cache disabled. */
        acr(ACR_COPYBACK); WORD(RESET_PROBE2) = RESET2; PHASE1 = 9;
        wait_phase(&PHASE0, 9);
        cacr(CACR_ON); OBS(RESET_RETAINED2) = WORD(PEER_DIRTY);
        check(OBS(RESET_RETAINED2), DIRTY2, 0x3014);
        invalidate(); OBS(RESET_INVALIDATED2) = WORD(PEER_DIRTY);
        check(OBS(RESET_INVALIDATED2), 0, 0x3015);
        PHASE1 = 10; finish(0x5555u);
    }
#endif
    check(boot, 1, 0x3000);
    cacr(0x01000000u); acr(ACR_COPYBACK); PHASE1 = 1;
    wait_phase(&PHASE0, 1);
    OBS(CACR_PEER) = WORD(DATA);
    check(OBS(CACR_PEER), OLD, 0x3001);
    acr(ACR_UNCACHED); cacr(CACR_ON); PHASE1 = 2;
    wait_phase(&PHASE0, 2);
    OBS(ACR_PEER) = WORD(DATA);
    check(OBS(ACR_PEER), OLD, 0x3002);
    WORD(PEER_PROBE) = PROBE;
    invalidate(); PHASE1 = 3;
    wait_phase(&PHASE0, 3);
    acr(ACR_COPYBACK); WORD(PEER_DIRTY) = DIRTY1;
    OBS(READER_DIRTY) = WORD(PEER_DIRTY);
    check(OBS(READER_DIRTY), DIRTY1, 0x3006);
    OBS(PRIMED) = WORD(DATA);
    check(OBS(PRIMED), OLD, 0x3003); PHASE1 = 4;
    wait_phase(&PHASE0, 4);
    OBS(STALE) = WORD(DATA);
    check(OBS(STALE), OLD, 0x3004);
#if DO_INVALIDATE
    invalidate();
#endif
    OBS(READER_AFTER_INVALIDATE) = WORD(PEER_DIRTY);
    check(OBS(READER_AFTER_INVALIDATE), DO_INVALIDATE ? 0 : DIRTY1, 0x3007);
    OBS(AFTER) = WORD(DATA);
    check(OBS(AFTER), DO_CLEAN && DO_INVALIDATE ? NEW2 : OLD, 0x3005);
    PHASE1 = 5;
    wait_phase(&PHASE0, 5);
#if TEST_RESET
    WORD(PEER_DIRTY) = DIRTY1; OBS(RESET_DIRTY) = WORD(PEER_DIRTY);
    check(OBS(RESET_DIRTY), DIRTY1, 0x3010); PHASE1 = 6;
    wait_for_rehold();
#else
    PHASE1 = 6; finish(0x5555u);
#endif
}
#endif
