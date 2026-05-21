/* CPU half of split VLA; static BSS (no malloc) for Vanadis.
 * Build: riscv64-linux-gnu-gcc -static -I.. -lm -o vla_cpu vla_cpu.c
 *
 * Phase-1 instrumentation: in addition to running the real CPU-side kernels
 * (vision_ingestion, concat_modalities, fast_dequantize, fast_idct), the
 * binary registers four labeled memory regions (weights / kv_cache /
 * activations / action_queue) via the HYADES_REGION_* MMIO ABI declared in
 * hyades.h. EccGuard's region-aware policy matches the workload-virtual
 * addresses Vanadis touches against the published regions, so faults can be
 * routed per-region (e.g. addr_filter_region=action_queue + Campaign).
 *
 * Each non-IDLE kernel call walks a handful of cache lines across the labeled
 * buffers it semantically dominates (mirroring stub_cpu.c's
 * kernel_region_mask) and folds the bytes it touches into a 32-bit
 * g_kernel_read_fold. The ACTUATE handler stamps a tick-keyed pattern into
 * action_queue, sweeps a buffer >> L2 to evict, cold-reads action_queue back
 * through the cache hierarchy (each refill traverses EccGuard), then publishes
 * (5381 ^ evict_sink ^ g_kernel_read_fold) folded with the cold-read bytes as
 * the per-frame action checksum via HYADES_ACTION_CHECKSUM. A SilentEscape on
 * any walked region byte during the frame, or on the action_queue refill at
 * actuate, therefore lands in the checksum and trips ActionScorer's
 * argmax_changed test against the BER=0 golden trajectory.
 */
#include "vla_shared.h"
#include <stdint.h>

/* Real-workload state (unchanged): exercised by the CPU kernels below and
 * also re-touched by walk_lines so faults on these arrays propagate through
 * the per-frame read fold. */
static float g_image[IMAGE_H * IMAGE_W * CHANNELS];
static float g_projected_tokens[NUM_PATCHES * LLM_DIM];
static float g_text_embeddings[NUM_TEXT_TOKENS * LLM_DIM];
static float g_unified_seq[(NUM_PATCHES + NUM_TEXT_TOKENS) * LLM_DIM];
static int   g_token_ids[NUM_ACTION_TOKENS];
static float g_token_dict[VOCAB_SIZE];
static float g_freq_matrix[ACTION_HORIZON * ACTION_DIM];
static float g_idct_weights[ACTION_HORIZON * ACTION_HORIZON];
static float g_continuous_action[ACTION_HORIZON * ACTION_DIM];

/* Phase-1 fault-injection scaffolding. Sizes are chosen so each labeled
 * buffer spans many cache lines (>> 64 B) and the action_queue + eviction
 * sweep together exceed the campaign-config CPU L2 (typically 32 KB when
 * watcher/campaign are enabled in testCarcosaVLA_GPUCPU.py), so the cold
 * readback at ACTUATE forces refills through EccGuard. */
#define VLA_BUF_WEIGHTS_SIZE      (16u * 1024u)
#define VLA_BUF_KV_CACHE_SIZE     (16u * 1024u)
#define VLA_BUF_ACTIVATIONS_SIZE  (16u * 1024u)
#define VLA_BUF_ACTION_QUEUE_SIZE ( 4u * 1024u)
#define VLA_BUF_EVICT_SWEEP_SIZE  (64u * 1024u)

static char g_weights      [VLA_BUF_WEIGHTS_SIZE];
static char g_kv_cache     [VLA_BUF_KV_CACHE_SIZE];
static char g_activations  [VLA_BUF_ACTIVATIONS_SIZE];
static char g_action_queue [VLA_BUF_ACTION_QUEUE_SIZE];
static char g_evict_sweep  [VLA_BUF_EVICT_SWEEP_SIZE];

#define VLA_REGION_SLOT_WEIGHTS       1
#define VLA_REGION_SLOT_KV_CACHE      2
#define VLA_REGION_SLOT_ACTIVATIONS   3
#define VLA_REGION_SLOT_ACTION_QUEUE  4

#define LINE_SIZE       64u
#define LINES_PER_TICK   4u

/* Per-frame fold of every byte the workload reads through walk_lines. An ECC
 * SilentEscape on a weights / kv_cache / activations line lands here BEFORE
 * the frame's actuate fold-and-publish runs, so any escape that flipped a
 * read-side bit in a prior kernel is mixed into the per-frame action
 * checksum. Reset at every ACTUATE close so the fold is per-frame, matching
 * the per-frame golden trajectory the ActionScorer compares against. */
static volatile unsigned int g_kernel_read_fold = 0u;

static unsigned g_tick = 0;

/* Read+touch LINES_PER_TICK lines of base; fold each byte into the per-frame
 * checksum and write the byte back so campaign-mode injection on writes can
 * fire when addr_filter_region matches this buffer's slot. Knuth
 * multiplicative mix (2654435761 = round(2**32 / phi)) so order and position
 * are visible in the final fold. */
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

static void idle_stub(void) { (void)0; }

static void vision_ingestion(void)
{
    size_t n = (size_t)IMAGE_H * IMAGE_W * CHANNELS * sizeof(float);
    for (size_t i = 0; i < n; i += 64)
        ((volatile char*)g_image)[i] = ((volatile char*)g_image)[i];
    walk_lines((volatile char *)g_activations, VLA_BUF_ACTIVATIONS_SIZE, g_tick);
}

static void concat_modalities(void)
{
    int nv = NUM_PATCHES, nt = NUM_TEXT_TOKENS, llm = LLM_DIM;
    for (int i = 0; i < nv; ++i)
        for (int d = 0; d < llm; ++d)
            g_unified_seq[i * llm + d] = g_projected_tokens[i * llm + d];
    for (int i = 0; i < nt; ++i)
        for (int d = 0; d < llm; ++d)
            g_unified_seq[(nv + i) * llm + d] = g_text_embeddings[i * llm + d];
    walk_lines((volatile char *)g_activations, VLA_BUF_ACTIVATIONS_SIZE, g_tick);
}

static void fast_dequantize(void)
{
    float scale_factor = 1.0f;
    for (int i = 0; i < NUM_ACTION_TOKENS; ++i) {
        int val = g_token_ids[i];
        float decompressed = g_token_dict[val] / scale_factor;
        int t = i / ACTION_DIM, d = i % ACTION_DIM;
        g_freq_matrix[t * ACTION_DIM + d] = decompressed;
    }
    walk_lines((volatile char *)g_weights,     VLA_BUF_WEIGHTS_SIZE,     g_tick);
    walk_lines((volatile char *)g_activations, VLA_BUF_ACTIVATIONS_SIZE, g_tick);
}

static void fast_idct(void)
{
    int horizon = ACTION_HORIZON, adim = ACTION_DIM;
    for (int d = 0; d < adim; ++d) {
        for (int t = 0; t < horizon; ++t) {
            float sum = 0.0f;
            for (int k = 0; k < horizon; ++k)
                sum += g_freq_matrix[k * adim + d] * g_idct_weights[t * horizon + k];
            g_continuous_action[t * adim + d] = sum;
        }
    }
    walk_lines((volatile char *)g_weights,     VLA_BUF_WEIGHTS_SIZE,     g_tick);
    walk_lines((volatile char *)g_activations, VLA_BUF_ACTIVATIONS_SIZE, g_tick);
}

/* Synthesize an "I am waiting on the GPU" call by walking the labeled
 * buffers a real GPU-resident kernel would dominate-access. Lets faults on
 * weights / kv_cache land within the read fold even when the corresponding
 * GPU kernel is the actual compute consumer. */
static void gpu_wait_weights_kv(void)
{
    walk_lines((volatile char *)g_weights,  VLA_BUF_WEIGHTS_SIZE,  g_tick);
    walk_lines((volatile char *)g_kv_cache, VLA_BUF_KV_CACHE_SIZE, g_tick);
}

static void gpu_wait_activations(void)
{
    walk_lines((volatile char *)g_activations, VLA_BUF_ACTIVATIONS_SIZE, g_tick);
}

/* ACTUATE close. The real-workload state (g_continuous_action) is mixed
 * into the published action_queue so a fault on either path (the real
 * idct floats OR the labeled action_queue refills) flips at least one
 * stamped byte. Sequence:
 *   1. Stamp tick-keyed payload + real action bytes into action_queue.
 *   2. Sweep g_evict_sweep (64 KB >> 32 KB campaign-config L2) to evict
 *      action_queue from L1/L2.
 *   3. Cold-read action_queue; each refill misses L2 and traverses
 *      EccGuard, so an injection on any action_queue line here flips the
 *      bytes we then fold into the checksum.
 *   4. Publish (5381 ^ evict_sink ^ g_kernel_read_fold) folded with the
 *      cold-read bytes as the per-frame action checksum.
 */
static void actuate(void)
{
    const char *cont = (const char *)g_continuous_action;
    const unsigned cont_n = (unsigned)sizeof(g_continuous_action);
    volatile char *aq = (volatile char *)g_action_queue;
    for (unsigned i = 0; i < VLA_BUF_ACTION_QUEUE_SIZE; ++i) {
        unsigned long lcg = (unsigned long)g_tick * 1103515245ul
                          + (unsigned long)i * 12345ul;
        aq[i] = (char)((unsigned char)cont[i % cont_n]
                     ^ (unsigned char)(lcg >> 8));
    }

    volatile char *vw = (volatile char *)g_evict_sweep;
    volatile unsigned int evict_sink = 0;
    for (unsigned round = 0; round < 4u; ++round)
        for (unsigned off = 0; off < VLA_BUF_EVICT_SWEEP_SIZE; off += LINE_SIZE)
            evict_sink ^= (unsigned int)(unsigned char)vw[off];

    unsigned int cs = 5381u ^ evict_sink ^ g_kernel_read_fold;
    g_kernel_read_fold = 0u;
    for (unsigned i = 0; i < VLA_BUF_ACTION_QUEUE_SIZE; ++i)
        cs = ((cs << 5) + cs) ^ (unsigned int)(unsigned char)aq[i];
    hyades_action_checksum_write(cs);
    ++g_tick;
}

static void gpu_wait_stub(void) { (void)0; }

/* Pre-touch every page of the labeled buffers so VanadisNodeOS does its
 * demand-paging up-front instead of charging 50-100 us per first-touch
 * fault during the first ACTUATE. */
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
    p = (volatile char *)g_evict_sweep;
    for (unsigned off = 0; off < VLA_BUF_EVICT_SWEEP_SIZE;  off += PAGE_SIZE) sink ^= p[off];
    (void)sink;
}

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    for (int i = 0; i < VOCAB_SIZE; ++i) g_token_dict[i] = 0.01f * (float)i;

    prefault_pages();

    /* Advertise the actual virtual addresses of the labeled buffers so
     * EccGuard's region-aware policy can route on the same addresses
     * Vanadis touches (MemEvent's preserved virtual address). The agent's
     * `regions` CSV supplies the symbolic name for each slot. */
    hyades_register_region(VLA_REGION_SLOT_WEIGHTS,
        (unsigned long)(uintptr_t)g_weights,      (unsigned long)VLA_BUF_WEIGHTS_SIZE);
    hyades_register_region(VLA_REGION_SLOT_KV_CACHE,
        (unsigned long)(uintptr_t)g_kv_cache,     (unsigned long)VLA_BUF_KV_CACHE_SIZE);
    hyades_register_region(VLA_REGION_SLOT_ACTIVATIONS,
        (unsigned long)(uintptr_t)g_activations,  (unsigned long)VLA_BUF_ACTIVATIONS_SIZE);
    hyades_register_region(VLA_REGION_SLOT_ACTION_QUEUE,
        (unsigned long)(uintptr_t)g_action_queue, (unsigned long)VLA_BUF_ACTION_QUEUE_SIZE);

    /* Kernel jump table. CPU runs real math for vision/concat/dequant/idct
     * and actuate; for kernels that are GPU-resident the CPU walks the
     * regions a real co-resident workload would touch, so faults can land
     * within those windows and propagate through the per-frame fold. */
    hyades_handler_t table[18];
    table[0]  = idle_stub;                /* IDLE */
    table[1]  = vision_ingestion;         /* VISION_INGESTION */
    table[2]  = gpu_wait_activations;     /* PATCHIFICATION_EMBED */
    table[3]  = gpu_wait_weights_kv;      /* VIS_ATTN_PROJ */
    table[4]  = gpu_wait_activations;     /* GLOBAL_SPATIAL_ATTN */
    table[5]  = gpu_wait_weights_kv;      /* VIS_FFN */
    table[6]  = gpu_wait_weights_kv;      /* MLP_PROJECTOR */
    table[7]  = concat_modalities;        /* SEQ_CONCAT */
    table[8]  = gpu_wait_weights_kv;      /* PREFILL_ATTN_PROJ */
    table[9]  = gpu_wait_weights_kv;      /* PREFILL_CAUSAL_ATTN */
    table[10] = gpu_wait_weights_kv;      /* PREFILL_FFN */
    table[11] = gpu_wait_weights_kv;      /* GEMV_PROJECT */
    table[12] = gpu_wait_weights_kv;      /* KV_CACHE_ATTN */
    table[13] = gpu_wait_weights_kv;      /* DECODE_FFN */
    table[14] = gpu_wait_weights_kv;      /* LM_HEAD */
    table[15] = fast_dequantize;          /* DETOK_DEQUANT */
    table[16] = fast_idct;                /* FAST_IDCT */
    table[17] = actuate;                  /* ACTUATE */

    (void)gpu_wait_stub;

    hyades_run(table, 18);
    return 0;
}
