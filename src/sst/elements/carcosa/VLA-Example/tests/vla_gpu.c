/* GPU half of split VLA; static BSS (no malloc) for Vanadis. Build: riscv64-linux-gnu-gcc -static -I.. -lm -o vla_gpu vla_gpu.c */
#include "vla_shared.h"

static float g_image[IMAGE_H * IMAGE_W * CHANNELS];
static float g_patch_weights[VIS_DIM * CHANNELS * PATCH_SIZE * PATCH_SIZE];
static float g_embeddings[NUM_PATCHES * VIS_DIM];
static float g_vis_qkv_weights[VIS_DIM * VIS_DIM * 3];
static float g_vis_Q[NUM_PATCHES * VIS_DIM];
static float g_vis_K[NUM_PATCHES * VIS_DIM];
static float g_vis_V[NUM_PATCHES * VIS_DIM];
static float g_vis_attn_out[NUM_PATCHES * VIS_DIM];
static float g_vis_ffn_w1[VIS_DIM * (4 * VIS_DIM)];
static float g_vis_ffn_w2[(4 * VIS_DIM) * VIS_DIM];

static float g_proj_w1[VIS_DIM * PROJ_HIDDEN];
static float g_proj_w2[PROJ_HIDDEN * LLM_DIM];
static float g_projected_tokens[NUM_PATCHES * LLM_DIM];

static float g_unified_seq[(NUM_PATCHES + NUM_TEXT_TOKENS) * LLM_DIM];
static float g_llm_qkv_w[LLM_DIM * (LLM_DIM * 3)];
static float g_llm_qkv_out[(NUM_PATCHES + NUM_TEXT_TOKENS) * (LLM_DIM * 3)];
static float g_prefill_Q[(NUM_PATCHES + NUM_TEXT_TOKENS) * LLM_DIM];
static float g_prefill_K[(NUM_PATCHES + NUM_TEXT_TOKENS) * LLM_DIM];
static float g_prefill_V[(NUM_PATCHES + NUM_TEXT_TOKENS) * LLM_DIM];
static float g_prefill_attn_out[(NUM_PATCHES + NUM_TEXT_TOKENS) * LLM_DIM];
static float g_prefill_ffn_w1[LLM_DIM * LLM_INTERMEDIATE];
static float g_prefill_ffn_w2[LLM_INTERMEDIATE * LLM_DIM];

static float g_token_vec[LLM_DIM];
static float g_wq[LLM_DIM * LLM_DIM];
static float g_wk[LLM_DIM * LLM_DIM];
static float g_wv[LLM_DIM * LLM_DIM];
static float g_q_vec[LLM_DIM];
static float g_k_vec[LLM_DIM];
static float g_v_vec[LLM_DIM];
static float g_k_cache[MAX_SEQ_LEN * LLM_DIM];
static float g_v_cache[MAX_SEQ_LEN * LLM_DIM];
static float g_decode_attn_out[LLM_DIM];
static float g_decode_ffn_w1[LLM_DIM * LLM_INTERMEDIATE];
static float g_decode_ffn_w2[LLM_INTERMEDIATE * LLM_DIM];

static float g_lm_head_w[LLM_DIM * VOCAB_SIZE];
static float g_logits[VOCAB_SIZE];

static float g_scratch_scores[MAX_SEQ_LEN];
static float g_scratch_vis_ffn_inter[NUM_PATCHES * (4 * VIS_DIM)];
static float g_scratch_mlp_inter[NUM_PATCHES * PROJ_HIDDEN];
static float g_scratch_prefill_inter[(NUM_PATCHES + NUM_TEXT_TOKENS) * LLM_INTERMEDIATE];
static float g_scratch_decode_inter[LLM_INTERMEDIATE];

static int g_current_seq_len;

static void cpu_wait_stub(void) { (void)0; }

static void patch_embedding(void)
{
    int H = IMAGE_H, W = IMAGE_W, C = CHANNELS, P = PATCH_SIZE, D = VIS_DIM;
    int nph = H / P, npw = W / P;
    for (int ph = 0; ph < nph; ++ph) {
        for (int pw = 0; pw < npw; ++pw) {
            int patch_idx = ph * npw + pw;
            for (int d = 0; d < D; ++d) {
                float sum = 0.0f;
                for (int c = 0; c < C; ++c) {
                    for (int i = 0; i < P; ++i) {
                        for (int j = 0; j < P; ++j) {
                            int py = ph * P + i, px = pw * P + j;
                            float pixel = g_image[(py * W + px) * C + c];
                            float w = g_patch_weights[(d * C * P * P) + (c * P * P) + (i * P) + j];
                            sum += pixel * w;
                        }
                    }
                }
                g_embeddings[patch_idx * D + d] = sum;
            }
        }
    }
}

static void vis_attn_projection(void)
{
    int D = VIS_DIM, d_k = D, np = NUM_PATCHES, tile = TILE_SIZE;
    memset(g_vis_Q, 0, (size_t)np * d_k * sizeof(float));
    memset(g_vis_K, 0, (size_t)np * d_k * sizeof(float));
    memset(g_vis_V, 0, (size_t)np * d_k * sizeof(float));
    tiled_gemm(g_embeddings, g_vis_qkv_weights, g_vis_Q, np, d_k, D, tile);
    tiled_gemm(g_embeddings, g_vis_qkv_weights + D * d_k, g_vis_K, np, d_k, D, tile);
    tiled_gemm(g_embeddings, g_vis_qkv_weights + 2 * D * d_k, g_vis_V, np, d_k, D, tile);
}

static void global_spatial_attention(void)
{
    int np = NUM_PATCHES, d_k = VIS_DIM;
    float scale = 1.0f / sqrtf((float)d_k);
    for (int i = 0; i < np; ++i) {
        float max_score = -1e30f;
        for (int j = 0; j < np; ++j) {
            float dot = 0.0f;
            for (int d = 0; d < d_k; ++d)
                dot += g_vis_Q[i * d_k + d] * g_vis_K[j * d_k + d];
            g_scratch_scores[j] = dot * scale;
            if (g_scratch_scores[j] > max_score) max_score = g_scratch_scores[j];
        }
        float sum_exp = 0.0f;
        for (int j = 0; j < np; ++j) {
            g_scratch_scores[j] = expf(g_scratch_scores[j] - max_score);
            sum_exp += g_scratch_scores[j];
        }
        for (int j = 0; j < np; ++j) g_scratch_scores[j] /= sum_exp;
        for (int d = 0; d < d_k; ++d) {
            float out_val = 0.0f;
            for (int j = 0; j < np; ++j)
                out_val += g_scratch_scores[j] * g_vis_V[j * d_k + d];
            g_vis_attn_out[i * d_k + d] = out_val;
        }
    }
}

static void vis_ffn(void)
{
    int np = NUM_PATCHES, D = VIS_DIM, tile = TILE_SIZE;
    memset(g_scratch_vis_ffn_inter, 0, (size_t)np * (4 * D) * sizeof(float));
    tiled_gemm(g_vis_attn_out, g_vis_ffn_w1, g_scratch_vis_ffn_inter, np, 4 * D, D, tile);
    for (int i = 0; i < np * (4 * D); ++i)
        g_scratch_vis_ffn_inter[i] = g_scratch_vis_ffn_inter[i] > 0.0f ? g_scratch_vis_ffn_inter[i] : 0.0f;
    memset(g_vis_attn_out, 0, (size_t)np * D * sizeof(float));
    tiled_gemm(g_scratch_vis_ffn_inter, g_vis_ffn_w2, g_vis_attn_out, np, D, 4 * D, tile);
}

static void mlp_projector(void)
{
    int np = NUM_PATCHES, vis_dim = VIS_DIM, hidden = PROJ_HIDDEN, llm = LLM_DIM, tile = TILE_SIZE;
    memset(g_scratch_mlp_inter, 0, (size_t)np * hidden * sizeof(float));
    tiled_gemm(g_vis_attn_out, g_proj_w1, g_scratch_mlp_inter, np, hidden, vis_dim, tile);
    {
        const float sqrt2_over_pi = 0.7978845608f;
        for (int i = 0; i < np * hidden; ++i) {
            float x = g_scratch_mlp_inter[i];
            g_scratch_mlp_inter[i] = 0.5f * x * (1.0f + tanhf(sqrt2_over_pi * (x + 0.044715f * x * x * x)));
        }
    }
    memset(g_projected_tokens, 0, (size_t)np * llm * sizeof(float));
    tiled_gemm(g_scratch_mlp_inter, g_proj_w2, g_projected_tokens, np, llm, hidden, tile);
}

static void prefill_attn_proj(void)
{
    int seq = NUM_PATCHES + NUM_TEXT_TOKENS, D = LLM_DIM, tile = TILE_SIZE;
    memset(g_llm_qkv_out, 0, (size_t)seq * (D * 3) * sizeof(float));
    tiled_gemm(g_unified_seq, g_llm_qkv_w, g_llm_qkv_out, seq, D * 3, D, tile);
}

static void prefill_causal_attn(void)
{
    int seq_len = NUM_PATCHES + NUM_TEXT_TOKENS, d_k = LLM_DIM;
    float scale = 1.0f / sqrtf((float)d_k);
    memcpy(g_prefill_Q, g_llm_qkv_out, (size_t)seq_len * d_k * sizeof(float));
    memcpy(g_prefill_K, g_llm_qkv_out + seq_len * d_k, (size_t)seq_len * d_k * sizeof(float));
    memcpy(g_prefill_V, g_llm_qkv_out + 2 * seq_len * d_k, (size_t)seq_len * d_k * sizeof(float));
    for (int i = 0; i < seq_len; ++i) {
        float max_score = -1e30f;
        for (int j = 0; j <= i; ++j) {
            float dot = 0.0f;
            for (int d = 0; d < d_k; ++d)
                dot += g_prefill_Q[i * d_k + d] * g_prefill_K[j * d_k + d];
            g_scratch_scores[j] = dot * scale;
            if (g_scratch_scores[j] > max_score) max_score = g_scratch_scores[j];
        }
        for (int j = i + 1; j < seq_len; ++j) g_scratch_scores[j] = -1e30f;
        float sum_exp = 0.0f;
        for (int j = 0; j <= i; ++j) {
            g_scratch_scores[j] = expf(g_scratch_scores[j] - max_score);
            sum_exp += g_scratch_scores[j];
        }
        for (int j = 0; j <= i; ++j) g_scratch_scores[j] /= sum_exp;
        for (int d = 0; d < d_k; ++d) {
            float out_val = 0.0f;
            for (int j = 0; j <= i; ++j)
                out_val += g_scratch_scores[j] * g_prefill_V[j * d_k + d];
            g_prefill_attn_out[i * d_k + d] = out_val;
        }
    }
}

static void prefill_ffn(void)
{
    int seq = NUM_PATCHES + NUM_TEXT_TOKENS, D = LLM_DIM, mid = LLM_INTERMEDIATE, tile = TILE_SIZE;
    memset(g_scratch_prefill_inter, 0, (size_t)seq * mid * sizeof(float));
    tiled_gemm(g_prefill_attn_out, g_prefill_ffn_w1, g_scratch_prefill_inter, seq, mid, D, tile);
    for (int i = 0; i < seq * mid; ++i) {
        float x = g_scratch_prefill_inter[i];
        g_scratch_prefill_inter[i] = x / (1.0f + expf(-x));
    }
    memset(g_prefill_attn_out, 0, (size_t)seq * D * sizeof(float));
    tiled_gemm(g_scratch_prefill_inter, g_prefill_ffn_w2, g_prefill_attn_out, seq, D, mid, tile);
}

static void gemv_project(void)
{
    int D = LLM_DIM;
    decode_gemv_kernel(g_token_vec, g_wq, g_q_vec, D, D);
    decode_gemv_kernel(g_token_vec, g_wk, g_k_vec, D, D);
    decode_gemv_kernel(g_token_vec, g_wv, g_v_vec, D, D);
}

static void kv_cache_attn(void)
{
    g_current_seq_len = hyades_seq_len_read();
    int seq_len = g_current_seq_len, d_k = LLM_DIM;
    for (int d = 0; d < d_k; ++d) {
        g_k_cache[(seq_len - 1) * d_k + d] = g_k_vec[d];
        g_v_cache[(seq_len - 1) * d_k + d] = g_v_vec[d];
    }
    float max_score = -1e30f;
    for (int j = 0; j < seq_len; ++j) {
        float dot = 0.0f;
        for (int d = 0; d < d_k; ++d)
            dot += g_q_vec[d] * g_k_cache[j * d_k + d];
        g_scratch_scores[j] = dot / sqrtf((float)d_k);
        if (g_scratch_scores[j] > max_score) max_score = g_scratch_scores[j];
    }
    float sum_exp = 0.0f;
    for (int j = 0; j < seq_len; ++j) {
        g_scratch_scores[j] = expf(g_scratch_scores[j] - max_score);
        sum_exp += g_scratch_scores[j];
    }
    for (int j = 0; j < seq_len; ++j) g_scratch_scores[j] /= sum_exp;
    for (int d = 0; d < d_k; ++d) {
        float out_val = 0.0f;
        for (int j = 0; j < seq_len; ++j)
            out_val += g_scratch_scores[j] * g_v_cache[j * d_k + d];
        g_decode_attn_out[d] = out_val;
    }
}

static void decode_ffn(void)
{
    int D = LLM_DIM, mid = LLM_INTERMEDIATE;
    decode_gemv_kernel(g_decode_attn_out, g_decode_ffn_w1, g_scratch_decode_inter, D, mid);
    for (int i = 0; i < mid; ++i) {
        float x = g_scratch_decode_inter[i];
        g_scratch_decode_inter[i] = x / (1.0f + expf(-x));
    }
    decode_gemv_kernel(g_scratch_decode_inter, g_decode_ffn_w2, g_token_vec, mid, D);
}

static void lm_head(void)
{
    decode_gemv_kernel(g_token_vec, g_lm_head_w, g_logits, LLM_DIM, VOCAB_SIZE);
}

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    hyades_handler_t table[18];
    table[0]  = cpu_wait_stub;
    table[1]  = cpu_wait_stub;
    table[2]  = patch_embedding;
    table[3]  = vis_attn_projection;
    table[4]  = global_spatial_attention;
    table[5]  = vis_ffn;
    table[6]  = mlp_projector;
    table[7]  = cpu_wait_stub;
    table[8]  = prefill_attn_proj;
    table[9]  = prefill_causal_attn;
    table[10] = prefill_ffn;
    table[11] = gemv_project;
    table[12] = kv_cache_attn;
    table[13] = decode_ffn;
    table[14] = lm_head;
    table[15] = cpu_wait_stub;
    table[16] = cpu_wait_stub;
    table[17] = cpu_wait_stub;

    hyades_run(table, 18);
    return 0;
}
