/* Phase 2 GPU memory-traffic generator stub.
 *
 * Mirrors stub_cpu.c: per-kernel walks over statically-allocated buffers, with
 * the binary publishing each buffer's virtual base via the HYADES_REGION_*
 * MMIO ABI. The GPU delay agent has an empty state_key in the default
 * testCarcosaVLA_GPUCPU_Synth.py topology, so its region-publish writes are
 * no-ops on the shared registry; the CPU agent owns the published view. Both
 * binaries still touch the same virtual addresses (their per-process page
 * tables map those virts to different phys pages but EccGuard matches on the
 * MemEvent's preserved virtual address, which is identical).
 */
#include "hyades.h"
#include <stdint.h>

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

#define MASK_WEIGHTS       (1u << 0)
#define MASK_KV_CACHE      (1u << 1)
#define MASK_ACTIVATIONS   (1u << 2)
#define MASK_ACTION_QUEUE  (1u << 3)

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

/* Mirrors stub_cpu.c::g_kernel_read_fold so the GPU's per-kernel reads also
 * propagate to its per-frame checksum. The GPU's HYADES_ACTION_CHECKSUM write
 * is ack-only on this topology (empty state_key on the GPU agent), but we
 * still mix the fold into cs so the GPU's payload at ACTUATE on the
 * memHierarchy side has the same shape as the CPU's. */
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

/* Mirrors stub_cpu's actuate_publish_checksum so both processes drive the
 * same payload through EccGuard at ACTUATE close. The GPU agent has an empty
 * state_key, so its 0x30 write is ack-only - only the CPU agent's published
 * checksum reaches PipelineStateBase::FrameRecord. We still do the writes
 * + eviction + readback here so the GPU's memory traffic at ACTUATE has the
 * same shape on both pipes (matters for parallel kernel attribution). */
static void actuate_publish_checksum(unsigned tick) {
    volatile char *aq = (volatile char *)g_action_queue;
    for (unsigned i = 0; i < VLA_BUF_ACTION_QUEUE_SIZE; ++i) {
        unsigned long lcg = (unsigned long)tick * 1103515245ul + (unsigned long)i * 12345ul;
        aq[i] = (char)(lcg >> 8);
    }
    volatile char *vw = (volatile char *)g_weights;
    volatile unsigned int evict_sink = 0;
    for (unsigned off = 0; off < VLA_BUF_WEIGHTS_SIZE; off += LINE_SIZE)
        evict_sink ^= (unsigned int)(unsigned char)vw[off];
    unsigned int cs = 5381u ^ evict_sink ^ g_kernel_read_fold;
    g_kernel_read_fold = 0u;
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
 * demand-paging up-front. See stub_cpu.c for rationale. */
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
