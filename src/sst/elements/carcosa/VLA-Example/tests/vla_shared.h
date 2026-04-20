#ifndef VLA_SHARED_H
#define VLA_SHARED_H

#include "hyades.h"
#include <math.h>
#include <string.h>

/* Dimensions: -D overrides; MAX_SEQ_LEN must match SST max_seq_len. */

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

static inline void tiled_gemm(float* A, float* B, float* C,
                               int M, int N, int K, int tile)
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

static inline void decode_gemv_kernel(float* in, float* W, float* out,
                                       int in_dim, int out_dim)
{
    for (int j = 0; j < out_dim; ++j) {
        float sum = 0.0f;
        for (int i = 0; i < in_dim; ++i)
            sum += in[i] * W[i * out_dim + j];
        out[j] = sum;
    }
}

#endif /* VLA_SHARED_H */
