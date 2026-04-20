/* CPU half of split VLA; static BSS (no malloc) for Vanadis. Build: riscv64-linux-gnu-gcc -static -I.. -lm -o vla_cpu vla_cpu.c */
#include "vla_shared.h"

static float g_image[IMAGE_H * IMAGE_W * CHANNELS];
static float g_projected_tokens[NUM_PATCHES * LLM_DIM];
static float g_text_embeddings[NUM_TEXT_TOKENS * LLM_DIM];
static float g_unified_seq[(NUM_PATCHES + NUM_TEXT_TOKENS) * LLM_DIM];
static int   g_token_ids[NUM_ACTION_TOKENS];
static float g_token_dict[VOCAB_SIZE];
static float g_freq_matrix[ACTION_HORIZON * ACTION_DIM];
static float g_idct_weights[ACTION_HORIZON * ACTION_HORIZON];
static float g_continuous_action[ACTION_HORIZON * ACTION_DIM];

static void idle_stub(void) { (void)0; }

static void vision_ingestion(void)
{
    size_t n = (size_t)IMAGE_H * IMAGE_W * CHANNELS * sizeof(float);
    for (size_t i = 0; i < n; i += 64)
        ((volatile char*)g_image)[i] = ((volatile char*)g_image)[i];
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
}

static void actuate(void) { (void)0; }

static void gpu_wait_stub(void) { (void)0; }

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    for (int i = 0; i < VOCAB_SIZE; ++i) g_token_dict[i] = 0.01f * (float)i;

    hyades_handler_t table[18];
    table[0]  = idle_stub;
    table[1]  = vision_ingestion;
    table[2]  = gpu_wait_stub;
    table[3]  = gpu_wait_stub;
    table[4]  = gpu_wait_stub;
    table[5]  = gpu_wait_stub;
    table[6]  = gpu_wait_stub;
    table[7]  = concat_modalities;
    table[8]  = gpu_wait_stub;
    table[9]  = gpu_wait_stub;
    table[10] = gpu_wait_stub;
    table[11] = gpu_wait_stub;
    table[12] = gpu_wait_stub;
    table[13] = gpu_wait_stub;
    table[14] = gpu_wait_stub;
    table[15] = fast_dequantize;
    table[16] = fast_idct;
    table[17] = actuate;

    hyades_run(table, 18);
    return 0;
}
