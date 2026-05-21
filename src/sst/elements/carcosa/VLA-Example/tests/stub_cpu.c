/* Phase 2 CPU memory-traffic generator stub.
 *
 * Drives the Hyades MMIO loop the same way the Phase 1 binary does, but
 * instead of running real kernel math, each kernel handler walks a per-kernel
 * set of cache lines across statically-allocated buffers that stand in for
 * the workload's labeled tensors / queues.
 *
 * At startup the binary registers each buffer's *virtual* base address with
 * the VLA delay agent via the HYADES_REGION_* MMIO ABI; the agent records
 * them into PipelineStateBase::regions[slot]. EccGuard then matches the
 * MemEvent's preserved virtual address against the published ranges, so
 * region-aware ECC policies see the same per-region accesses Vanadis
 * actually generates (at a representative cadence, not the full workload
 * memory footprint).
 *
 * Slot ids must match the agent's `regions` CSV (defaults in
 * run_ecc_sweep.sh REGIONS_CSV): 1=weights, 2=kv_cache, 3=activations,
 * 4=action_queue.
 */
#include "hyades.h"
#include <stdint.h>

/* Buffer sizes are intentionally compact so Vanadis demand-paging stays cheap;
 * each kernel call only touches a few cache lines. The agent publishes the
 * actual sizes via the HYADES_REGION_* commit, so EccGuard's range check
 * matches the touched window exactly. */
#define VLA_BUF_WEIGHTS_SIZE      (64u * 1024u)
#define VLA_BUF_KV_CACHE_SIZE     (64u * 1024u)
#define VLA_BUF_ACTIVATIONS_SIZE  (64u * 1024u)
#define VLA_BUF_ACTION_QUEUE_SIZE ( 4u * 1024u)

static char g_weights     [VLA_BUF_WEIGHTS_SIZE];
static char g_kv_cache    [VLA_BUF_KV_CACHE_SIZE];
static char g_activations [VLA_BUF_ACTIVATIONS_SIZE];
static char g_action_queue[VLA_BUF_ACTION_QUEUE_SIZE];

#define VLA_REGION_SLOT_WEIGHTS       1
#define VLA_REGION_SLOT_KV_CACHE      2
#define VLA_REGION_SLOT_ACTIVATIONS   3
#define VLA_REGION_SLOT_ACTION_QUEUE  4

/* Per-kernel region masks. Each bit selects one of the buffers above. */
#define MASK_WEIGHTS       (1u << 0)
#define MASK_KV_CACHE      (1u << 1)
#define MASK_ACTIVATIONS   (1u << 2)
#define MASK_ACTION_QUEUE  (1u << 3)

/* 18 kernel ids from vla-fsm.h. Mirrors the EccGuard region_aware policy in
 * run_ecc_sweep.sh: each kernel walks the buffers the real workload would
 * dominate-access (so per-region BERs and per-(kernel, region) overrides fire
 * on representative addresses). */
static const unsigned char kernel_region_mask[18] = {
    /*  0 IDLE                 */ 0,
    /*  1 VISION_INGESTION     */ MASK_ACTIVATIONS,
    /*  2 PATCHIFICATION_EMBED */ MASK_WEIGHTS | MASK_ACTIVATIONS,
    /*  3 VIS_ATTN_PROJ        */ MASK_WEIGHTS | MASK_ACTIVATIONS,
    /*  4 GLOBAL_SPATIAL_ATTN  */ MASK_ACTIVATIONS,
    /*  5 VIS_FFN              */ MASK_WEIGHTS | MASK_ACTIVATIONS,
    /*  6 MLP_PROJECTOR        */ MASK_WEIGHTS | MASK_ACTIVATIONS,
    /*  7 SEQ_CONCAT           */ MASK_ACTIVATIONS,
    /*  8 PREFILL_ATTN_PROJ    */ MASK_WEIGHTS | MASK_ACTIVATIONS,
    /*  9 PREFILL_CAUSAL_ATTN  */ MASK_KV_CACHE | MASK_ACTIVATIONS,
    /* 10 PREFILL_FFN          */ MASK_WEIGHTS | MASK_ACTIVATIONS,
    /* 11 GEMV_PROJECT         */ MASK_WEIGHTS | MASK_ACTIVATIONS,
    /* 12 KV_CACHE_ATTN        */ MASK_KV_CACHE | MASK_ACTIVATIONS,
    /* 13 DECODE_FFN           */ MASK_WEIGHTS | MASK_ACTIVATIONS,
    /* 14 LM_HEAD              */ MASK_WEIGHTS | MASK_ACTIVATIONS,
    /* 15 DETOK_DEQUANT        */ MASK_ACTIVATIONS,
    /* 16 FAST_IDCT            */ MASK_ACTIVATIONS,
    /* 17 ACTUATE              */ MASK_ACTION_QUEUE,
};

#define LINE_SIZE       64u
#define LINES_PER_TICK   4u

/* Per-frame running fold of every byte the workload reads through walk_lines.
 * An ECC SilentEscape that flips a bit on a weights / kv_cache / activations
 * line (i.e. anywhere outside ACTUATE's action_queue) lands here BEFORE the
 * frame's actuate_publish_checksum runs, and is then mixed into the action
 * checksum. Without this fold, only escapes on the action_queue refill would
 * change the checksum and the headline pressure-point graph collapses to zero
 * for every BER below the (very rare) action_queue escape rate.
 *
 * Knuth multiplicative mix (2654435761 = round(2**32 / phi)): order- and
 * position-sensitive, so a flip in any byte read in any kernel is visible in
 * the final 32-bit fold. Reset at every ACTUATE close so the fold is per-
 * frame, matching the per-frame golden trajectory the ActionScorer compares
 * against. */
static volatile unsigned int g_kernel_read_fold = 0u;

static inline void walk_lines(volatile char *base, unsigned size,
                              unsigned start_line) {
    for (unsigned i = 0; i < LINES_PER_TICK; ++i) {
        unsigned off = ((start_line + i) * LINE_SIZE) % size;
        char v = base[off];
        g_kernel_read_fold = g_kernel_read_fold * 2654435761u
                           ^ (unsigned int)(unsigned char)v;
        base[off] = (char)(v + 1);
    }
}

static unsigned g_tick = 0;

/* ACTUATE close: build a kernel-dependent payload in action_queue, force-evict
 * it from L1 by sweeping a working set larger than L1d, then read the queue
 * back through the cache hierarchy and fold into a 32-bit checksum. Because
 * the readback misses L1 and traverses EccGuard, a SilentEscape on the
 * action_queue refill (or on a previously-evicted writeback) shows up in the
 * checksum. The result is published via HYADES_ACTION_CHECKSUM, which the
 * delay agent stamps onto the next FrameRecord. */
static void actuate_publish_checksum(unsigned tick) {
    volatile char *aq = (volatile char *)g_action_queue;
    /* Stamp a fresh, tick-keyed pattern; in a real workload this would be
     * the emitted action tokens. Using a multiplicative LCG keeps every byte
     * tick-dependent so an Escape bit-flip can't be hidden by zero bytes. */
    for (unsigned i = 0; i < VLA_BUF_ACTION_QUEUE_SIZE; ++i) {
        unsigned long lcg = (unsigned long)tick * 1103515245ul + (unsigned long)i * 12345ul;
        aq[i] = (char)(lcg >> 8);
    }
    /* Evict the action_queue lines from L1d by streaming through g_weights
     * (64 KB > 32 KB L1d, with comfortable headroom for the 8-way set
     * mapping). Earlier kernels already demand-paged g_weights, so the cold
     * reads here pay L1-miss cost but not page-fault cost. volatile casts
     * prevent the compiler from collapsing the loop into a no-op. */
    volatile char *vw = (volatile char *)g_weights;
    volatile unsigned int evict_sink = 0;
    /* Sweep >> L2 (1 MiB in the synth config) so action_queue refills cannot
     * be satisfied from L2 after the tick-keyed fill. */
    for (unsigned round = 0; round < 20u; ++round)
        for (unsigned off = 0; off < VLA_BUF_WEIGHTS_SIZE; off += LINE_SIZE)
            evict_sink ^= (unsigned int)(unsigned char)vw[off];
    /* Mix in the running fold of every byte the prior kernels in this frame
     * read through walk_lines. This is the path by which an escape on a
     * weights / kv_cache / activations line reaches the action checksum;
     * without it only action_queue escapes would ever flip cs. */
    unsigned int cs = 5381u ^ evict_sink ^ g_kernel_read_fold;
    g_kernel_read_fold = 0u;
    /* Cold-read the action_queue back; these loads miss L1 and travel
     * through EccGuard, so any SilentEscape on the refill flips bytes here. */
    for (unsigned i = 0; i < VLA_BUF_ACTION_QUEUE_SIZE; ++i)
        cs = ((cs << 5) + cs) ^ (unsigned int)(unsigned char)aq[i];
    hyades_action_checksum_write(cs);
}

static void kernel_dispatch(int idx) {
    if (idx < 0 || idx >= 18) return;
    if (idx == 17 /* ACTUATE - see vla-fsm.h */) {
        actuate_publish_checksum(g_tick);
        ++g_tick;
        return;
    }
    unsigned mask = kernel_region_mask[idx];
    if (mask == 0) return;
    if (mask & MASK_WEIGHTS)
        walk_lines((volatile char *)g_weights,     VLA_BUF_WEIGHTS_SIZE,     g_tick);
    if (mask & MASK_KV_CACHE)
        walk_lines((volatile char *)g_kv_cache,    VLA_BUF_KV_CACHE_SIZE,    g_tick);
    if (mask & MASK_ACTIVATIONS)
        walk_lines((volatile char *)g_activations, VLA_BUF_ACTIVATIONS_SIZE, g_tick);
    if (mask & MASK_ACTION_QUEUE)
        walk_lines((volatile char *)g_action_queue,VLA_BUF_ACTION_QUEUE_SIZE,g_tick);
    ++g_tick;
}

/* Pre-touch every page of the four BSS buffers so VanadisNodeOS does its
 * demand-paging up-front instead of charging us 50-100 us per first-touch
 * fault during a hot kernel like ACTUATE. Without this, the ACTUATE
 * eviction sweep + cold readback can dominate sim time for the FIRST
 * pipeline cycle by triggering many page faults at once. */
#define PAGE_SIZE 4096u
static void prefault_pages(void) {
    volatile unsigned int sink = 0;
    volatile char *p;
    p = (volatile char *)g_weights;
    for (unsigned off = 0; off < VLA_BUF_WEIGHTS_SIZE;      off += PAGE_SIZE) sink ^= p[off];
    p = (volatile char *)g_kv_cache;
    for (unsigned off = 0; off < VLA_BUF_KV_CACHE_SIZE;     off += PAGE_SIZE) sink ^= p[off];
    p = (volatile char *)g_activations;
    for (unsigned off = 0; off < VLA_BUF_ACTIVATIONS_SIZE;  off += PAGE_SIZE) sink ^= p[off];
    p = (volatile char *)g_action_queue;
    for (unsigned off = 0; off < VLA_BUF_ACTION_QUEUE_SIZE; off += PAGE_SIZE) sink ^= p[off];
    (void)sink;
}

int main(void)
{
    prefault_pages();

    /* Advertise the actual virtual addresses of these buffers so EccGuard's
     * region-aware policy can route on the same addresses Vanadis touches.
     * The agent preserves the symbolic name it parsed from `regions` CSV. */
    hyades_register_region(VLA_REGION_SLOT_WEIGHTS,
        (unsigned long)(uintptr_t)g_weights,      (unsigned long)VLA_BUF_WEIGHTS_SIZE);
    hyades_register_region(VLA_REGION_SLOT_KV_CACHE,
        (unsigned long)(uintptr_t)g_kv_cache,     (unsigned long)VLA_BUF_KV_CACHE_SIZE);
    hyades_register_region(VLA_REGION_SLOT_ACTIVATIONS,
        (unsigned long)(uintptr_t)g_activations,  (unsigned long)VLA_BUF_ACTIVATIONS_SIZE);
    hyades_register_region(VLA_REGION_SLOT_ACTION_QUEUE,
        (unsigned long)(uintptr_t)g_action_queue, (unsigned long)VLA_BUF_ACTION_QUEUE_SIZE);

    hyades_run_idx(kernel_dispatch);
    return 0;
}
