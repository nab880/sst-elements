#include "hyades.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef IMAGE_H
#define IMAGE_H 8
#endif
#ifndef IMAGE_W
#define IMAGE_W 8
#endif
#ifndef CHANNELS
#define CHANNELS 3
#endif
#ifndef PATCH_SIZE
#define PATCH_SIZE 4
#endif
#ifndef VIS_DIM
#define VIS_DIM 64
#endif
#define NUM_PATCHES_H (IMAGE_H / PATCH_SIZE)
#define NUM_PATCHES_W (IMAGE_W / PATCH_SIZE)
#define NUM_PATCHES (NUM_PATCHES_H * NUM_PATCHES_W)

#ifndef LLM_DIM
#define LLM_DIM 128
#endif
#ifndef LLM_INTERMEDIATE
#define LLM_INTERMEDIATE 256
#endif
#ifndef VOCAB_SIZE
#define VOCAB_SIZE 256
#endif
#ifndef NUM_TEXT_TOKENS
#define NUM_TEXT_TOKENS 4
#endif
#ifndef PROJ_HIDDEN
#define PROJ_HIDDEN 128
#endif
#ifndef ACTION_HORIZON
#define ACTION_HORIZON 4
#endif
#ifndef ACTION_DIM
#define ACTION_DIM 2
#endif
#ifndef NUM_ACTION_TOKENS
#define NUM_ACTION_TOKENS (ACTION_HORIZON * ACTION_DIM)
#endif
#ifndef TILE_SIZE
#define TILE_SIZE 8
#endif
#ifndef MAX_SEQ_LEN
#define MAX_SEQ_LEN 64
#endif

#define MIN(a, b) ((a) < (b) ? (a) : (b))

static float* g_image;
static float* g_patch_weights;
static float* g_embeddings;
static float* g_vis_qkv_weights;
static float* g_vis_Q;
static float* g_vis_K;
static float* g_vis_V;
static float* g_vis_attn_out;
static float* g_vis_ffn_w1;
static float* g_vis_ffn_w2;
static float* g_proj_w1;
static float* g_proj_w2;
static float* g_projected_tokens;
static float* g_text_embeddings;
static float* g_unified_seq;
static float* g_llm_qkv_w;
static float* g_llm_qkv_out;
static float* g_prefill_Q;
static float* g_prefill_K;
static float* g_prefill_V;
static float* g_prefill_attn_out;
static float* g_prefill_ffn_w1;
static float* g_prefill_ffn_w2;
static float* g_token_vec;
static float* g_wq;
static float* g_wk;
static float* g_wv;
static float* g_q_vec;
static float* g_k_vec;
static float* g_v_vec;
static float* g_k_cache;
static float* g_v_cache;
static float* g_decode_attn_out;
static float* g_decode_ffn_w1;
static float* g_decode_ffn_w2;
static float* g_lm_head_w;
static float* g_logits;
static int* g_token_ids;
static float* g_token_dict;
static float* g_freq_matrix;
static float* g_idct_weights;
static float* g_continuous_action;
static int g_current_seq_len;

static void tiled_gemm(float* A, float* B, float* C, int M, int N, int K, int tile)
{
    for (int i = 0; i < M; i += tile) {
        for (int j = 0; j < N; j += tile) {
            for (int k = 0; k < K; k += tile) {
                for (int ii = i; ii < MIN(i + tile, M); ++ii) {
                    for (int jj = j; jj < MIN(j + tile, N); ++jj) {
                        float sum = 0.0f;
                        for (int kk = k; kk < MIN(k + tile, K); ++kk)
                            sum += A[ii * K + kk] * B[kk * N + jj];
                        C[ii * N + jj] += sum;
                    }
                }
            }
        }
    }
}

static void decode_gemv_kernel(float* in, float* W, float* out, int in_dim, int out_dim)
{
    for (int j = 0; j < out_dim; ++j) {
        float sum = 0.0f;
        for (int i = 0; i < in_dim; ++i)
            sum += in[i] * W[i * out_dim + j];
        out[j] = sum;
    }
}

static void idle_stub(void) { (void)0; }

static void vision_ingestion(void)
{
    size_t n = (size_t)IMAGE_H * IMAGE_W * CHANNELS * sizeof(float);
    for (size_t i = 0; i < n; i += 64)
        ((volatile char*)g_image)[i] = ((volatile char*)g_image)[i];
}

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
    float* scores = (float*)malloc((size_t)np * sizeof(float));
    for (int i = 0; i < np; ++i) {
        float max_score = -1e30f;
        for (int j = 0; j < np; ++j) {
            float dot = 0.0f;
            for (int d = 0; d < d_k; ++d)
                dot += g_vis_Q[i * d_k + d] * g_vis_K[j * d_k + d];
            scores[j] = dot * scale;
            if (scores[j] > max_score) max_score = scores[j];
        }
        float sum_exp = 0.0f;
        for (int j = 0; j < np; ++j) {
            scores[j] = expf(scores[j] - max_score);
            sum_exp += scores[j];
        }
        for (int j = 0; j < np; ++j) scores[j] /= sum_exp;
        for (int d = 0; d < d_k; ++d) {
            float out_val = 0.0f;
            for (int j = 0; j < np; ++j)
                out_val += scores[j] * g_vis_V[j * d_k + d];
            g_vis_attn_out[i * d_k + d] = out_val;
        }
    }
    free(scores);
}

static void vis_ffn(void)
{
    int np = NUM_PATCHES, D = VIS_DIM, tile = TILE_SIZE;
    float* inter = (float*)malloc((size_t)np * (4 * D) * sizeof(float));
    memset(inter, 0, (size_t)np * (4 * D) * sizeof(float));
    tiled_gemm(g_vis_attn_out, g_vis_ffn_w1, inter, np, 4 * D, D, tile);
    for (int i = 0; i < np * (4 * D); ++i) inter[i] = inter[i] > 0.0f ? inter[i] : 0.0f;
    memset(g_vis_attn_out, 0, (size_t)np * D * sizeof(float));
    tiled_gemm(inter, g_vis_ffn_w2, g_vis_attn_out, np, D, 4 * D, tile);
    free(inter);
}

static void mlp_projector(void)
{
    int np = NUM_PATCHES, vis_dim = VIS_DIM, hidden = PROJ_HIDDEN, llm = LLM_DIM, tile = TILE_SIZE;
    float* inter = (float*)malloc((size_t)np * hidden * sizeof(float));
    memset(inter, 0, (size_t)np * hidden * sizeof(float));
    tiled_gemm(g_vis_attn_out, g_proj_w1, inter, np, hidden, vis_dim, tile);
    {
        const float sqrt2_over_pi = 0.7978845608f;
        for (int i = 0; i < np * hidden; ++i) {
            float x = inter[i];
            inter[i] = 0.5f * x * (1.0f + tanhf(sqrt2_over_pi * (x + 0.044715f * x * x * x)));
        }
    }
    memset(g_projected_tokens, 0, (size_t)np * llm * sizeof(float));
    tiled_gemm(inter, g_proj_w2, g_projected_tokens, np, llm, hidden, tile);
    free(inter);
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
    float* scores = (float*)malloc((size_t)seq_len * sizeof(float));
    for (int i = 0; i < seq_len; ++i) {
        float max_score = -1e30f;
        for (int j = 0; j <= i; ++j) {
            float dot = 0.0f;
            for (int d = 0; d < d_k; ++d)
                dot += g_prefill_Q[i * d_k + d] * g_prefill_K[j * d_k + d];
            scores[j] = dot * scale;
            if (scores[j] > max_score) max_score = scores[j];
        }
        for (int j = i + 1; j < seq_len; ++j) scores[j] = -1e30f;
        float sum_exp = 0.0f;
        for (int j = 0; j <= i; ++j) {
            scores[j] = expf(scores[j] - max_score);
            sum_exp += scores[j];
        }
        for (int j = 0; j <= i; ++j) scores[j] /= sum_exp;
        for (int d = 0; d < d_k; ++d) {
            float out_val = 0.0f;
            for (int j = 0; j <= i; ++j)
                out_val += scores[j] * g_prefill_V[j * d_k + d];
            g_prefill_attn_out[i * d_k + d] = out_val;
        }
    }
    free(scores);
}

static void prefill_ffn(void)
{
    int seq = NUM_PATCHES + NUM_TEXT_TOKENS, D = LLM_DIM, mid = LLM_INTERMEDIATE, tile = TILE_SIZE;
    float* inter = (float*)malloc((size_t)seq * mid * sizeof(float));
    memset(inter, 0, (size_t)seq * mid * sizeof(float));
    tiled_gemm(g_prefill_attn_out, g_prefill_ffn_w1, inter, seq, mid, D, tile);
    for (int i = 0; i < seq * mid; ++i) {
        float x = inter[i];
        inter[i] = x / (1.0f + expf(-x));
    }
    memset(g_prefill_attn_out, 0, (size_t)seq * D * sizeof(float));
    tiled_gemm(inter, g_prefill_ffn_w2, g_prefill_attn_out, seq, D, mid, tile);
    free(inter);
}

static void gemv_project(void)
{
    int D = LLM_DIM;
    decode_gemv_kernel(g_token_vec, g_wq, g_q_vec, D, D);
    decode_gemv_kernel(g_token_vec, g_wk, g_k_vec, D, D);
    decode_gemv_kernel(g_token_vec, g_wv, g_v_vec, D, D);
}

#define HYADES_SEQ_LEN_OFFSET 8
static void kv_cache_attn(void)
{
    g_current_seq_len = *(volatile int*)(HYADES_MMIO_BASE + HYADES_SEQ_LEN_OFFSET);
    int seq_len = g_current_seq_len, d_k = LLM_DIM;
    for (int d = 0; d < d_k; ++d) {
        g_k_cache[(seq_len - 1) * d_k + d] = g_k_vec[d];
        g_v_cache[(seq_len - 1) * d_k + d] = g_v_vec[d];
    }
    float* scores = (float*)malloc((size_t)seq_len * sizeof(float));
    float max_score = -1e30f;
    for (int j = 0; j < seq_len; ++j) {
        float dot = 0.0f;
        for (int d = 0; d < d_k; ++d)
            dot += g_q_vec[d] * g_k_cache[j * d_k + d];
        scores[j] = dot / sqrtf((float)d_k);
        if (scores[j] > max_score) max_score = scores[j];
    }
    float sum_exp = 0.0f;
    for (int j = 0; j < seq_len; ++j) {
        scores[j] = expf(scores[j] - max_score);
        sum_exp += scores[j];
    }
    for (int j = 0; j < seq_len; ++j) scores[j] /= sum_exp;
    for (int d = 0; d < d_k; ++d) {
        float out_val = 0.0f;
        for (int j = 0; j < seq_len; ++j)
            out_val += scores[j] * g_v_cache[j * d_k + d];
        g_decode_attn_out[d] = out_val;
    }
    free(scores);
}

static void decode_ffn(void)
{
    int D = LLM_DIM, mid = LLM_INTERMEDIATE;
    float* inter = (float*)malloc((size_t)mid * sizeof(float));
    decode_gemv_kernel(g_decode_attn_out, g_decode_ffn_w1, inter, D, mid);
    for (int i = 0; i < mid; ++i) {
        float x = inter[i];
        inter[i] = x / (1.0f + expf(-x));
    }
    decode_gemv_kernel(inter, g_decode_ffn_w2, g_token_vec, mid, D);
    free(inter);
}

static void lm_head(void)
{
    decode_gemv_kernel(g_token_vec, g_lm_head_w, g_logits, LLM_DIM, VOCAB_SIZE);
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

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;
    size_t sz;
    g_image = (float*)malloc((sz = (size_t)IMAGE_H * IMAGE_W * CHANNELS * sizeof(float)));
    memset(g_image, 0, sz);
    g_patch_weights = (float*)malloc((sz = (size_t)VIS_DIM * CHANNELS * PATCH_SIZE * PATCH_SIZE * sizeof(float)));
    memset(g_patch_weights, 0, sz);
    g_embeddings = (float*)malloc((sz = (size_t)NUM_PATCHES * VIS_DIM * sizeof(float)));
    memset(g_embeddings, 0, sz);
    g_vis_qkv_weights = (float*)malloc((sz = (size_t)VIS_DIM * VIS_DIM * 3 * sizeof(float)));
    memset(g_vis_qkv_weights, 0, sz);
    g_vis_Q = (float*)malloc((sz = (size_t)NUM_PATCHES * VIS_DIM * sizeof(float)));
    g_vis_K = (float*)malloc(sz);
    g_vis_V = (float*)malloc(sz);
    g_vis_attn_out = (float*)malloc(sz);
    g_vis_ffn_w1 = (float*)malloc((sz = (size_t)VIS_DIM * (4 * VIS_DIM) * sizeof(float)));
    g_vis_ffn_w2 = (float*)malloc((sz = (size_t)(4 * VIS_DIM) * VIS_DIM * sizeof(float)));
    g_proj_w1 = (float*)malloc((sz = (size_t)VIS_DIM * PROJ_HIDDEN * sizeof(float)));
    g_proj_w2 = (float*)malloc((sz = (size_t)PROJ_HIDDEN * LLM_DIM * sizeof(float)));
    g_projected_tokens = (float*)malloc((sz = (size_t)NUM_PATCHES * LLM_DIM * sizeof(float)));
    g_text_embeddings = (float*)malloc((sz = (size_t)NUM_TEXT_TOKENS * LLM_DIM * sizeof(float)));
    g_unified_seq = (float*)malloc((sz = (size_t)(NUM_PATCHES + NUM_TEXT_TOKENS) * LLM_DIM * sizeof(float)));
    g_llm_qkv_w = (float*)malloc((sz = (size_t)LLM_DIM * (LLM_DIM * 3) * sizeof(float)));
    g_llm_qkv_out = (float*)malloc((sz = (size_t)(NUM_PATCHES + NUM_TEXT_TOKENS) * (LLM_DIM * 3) * sizeof(float)));
    g_prefill_Q = (float*)malloc((sz = (size_t)(NUM_PATCHES + NUM_TEXT_TOKENS) * LLM_DIM * sizeof(float)));
    g_prefill_K = (float*)malloc(sz);
    g_prefill_V = (float*)malloc(sz);
    g_prefill_attn_out = (float*)malloc(sz);
    g_prefill_ffn_w1 = (float*)malloc((sz = (size_t)LLM_DIM * LLM_INTERMEDIATE * sizeof(float)));
    g_prefill_ffn_w2 = (float*)malloc((sz = (size_t)LLM_INTERMEDIATE * LLM_DIM * sizeof(float)));
    g_token_vec = (float*)malloc((sz = (size_t)LLM_DIM * sizeof(float)));
    g_wq = (float*)malloc((sz = (size_t)LLM_DIM * LLM_DIM * sizeof(float)));
    g_wk = (float*)malloc(sz);
    g_wv = (float*)malloc(sz);
    g_q_vec = (float*)malloc((sz = (size_t)LLM_DIM * sizeof(float)));
    g_k_vec = (float*)malloc(sz);
    g_v_vec = (float*)malloc(sz);
    g_k_cache = (float*)malloc((sz = (size_t)MAX_SEQ_LEN * LLM_DIM * sizeof(float)));
    g_v_cache = (float*)malloc(sz);
    g_decode_attn_out = (float*)malloc((sz = (size_t)LLM_DIM * sizeof(float)));
    g_decode_ffn_w1 = (float*)malloc((sz = (size_t)LLM_DIM * LLM_INTERMEDIATE * sizeof(float)));
    g_decode_ffn_w2 = (float*)malloc((sz = (size_t)LLM_INTERMEDIATE * LLM_DIM * sizeof(float)));
    g_lm_head_w = (float*)malloc((sz = (size_t)LLM_DIM * VOCAB_SIZE * sizeof(float)));
    g_logits = (float*)malloc((sz = (size_t)VOCAB_SIZE * sizeof(float)));
    g_token_ids = (int*)malloc((sz = (size_t)NUM_ACTION_TOKENS * sizeof(int)));
    g_token_dict = (float*)malloc((sz = (size_t)VOCAB_SIZE * sizeof(float)));
    g_freq_matrix = (float*)malloc((sz = (size_t)ACTION_HORIZON * ACTION_DIM * sizeof(float)));
    g_idct_weights = (float*)malloc((sz = (size_t)ACTION_HORIZON * ACTION_HORIZON * sizeof(float)));
    g_continuous_action = (float*)malloc((sz = (size_t)ACTION_HORIZON * ACTION_DIM * sizeof(float)));

    g_current_seq_len = 1;
    for (int i = 0; i < VOCAB_SIZE; ++i) g_token_dict[i] = 0.01f * (float)i;

    hyades_handler_t jump_table[18];
    jump_table[0] = idle_stub;
    jump_table[1] = vision_ingestion;
    jump_table[2] = patch_embedding;
    jump_table[3] = vis_attn_projection;
    jump_table[4] = global_spatial_attention;
    jump_table[5] = vis_ffn;
    jump_table[6] = mlp_projector;
    jump_table[7] = concat_modalities;
    jump_table[8] = prefill_attn_proj;
    jump_table[9] = prefill_causal_attn;
    jump_table[10] = prefill_ffn;
    jump_table[11] = gemv_project;
    jump_table[12] = kv_cache_attn;
    jump_table[13] = decode_ffn;
    jump_table[14] = lm_head;
    jump_table[15] = fast_dequantize;
    jump_table[16] = fast_idct;
    jump_table[17] = actuate;

    hyades_run(jump_table, 18);

    free(g_continuous_action);
    free(g_idct_weights);
    free(g_freq_matrix);
    free(g_token_dict);
    free(g_token_ids);
    free(g_logits);
    free(g_lm_head_w);
    free(g_decode_ffn_w2);
    free(g_decode_ffn_w1);
    free(g_decode_attn_out);
    free(g_v_cache);
    free(g_k_cache);
    free(g_v_vec);
    free(g_k_vec);
    free(g_q_vec);
    free(g_wv);
    free(g_wk);
    free(g_wq);
    free(g_token_vec);
    free(g_prefill_ffn_w2);
    free(g_prefill_ffn_w1);
    free(g_prefill_attn_out);
    free(g_prefill_V);
    free(g_prefill_K);
    free(g_prefill_Q);
    free(g_llm_qkv_out);
    free(g_llm_qkv_w);
    free(g_unified_seq);
    free(g_text_embeddings);
    free(g_projected_tokens);
    free(g_proj_w2);
    free(g_proj_w1);
    free(g_vis_ffn_w2);
    free(g_vis_ffn_w1);
    free(g_vis_attn_out);
    free(g_vis_V);
    free(g_vis_K);
    free(g_vis_Q);
    free(g_vis_qkv_weights);
    free(g_embeddings);
    free(g_patch_weights);
    free(g_image);
    return 0;
}
