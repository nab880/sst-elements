// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.
//
// Portions are copyright of other developers:
// See the file CONTRIBUTORS.TXT in the top level directory
// of the distribution for more information.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.
//
// fft.cu — GPU-driven radix-2 Cooley-Tukey FFT for the Quetz/Balar demo.
//
// Two functionally-identical implementations are provided so the architectural
// demo can contrast how kernel decomposition shifts work between the modeled
// host fabric (scratch writes / flushes / doorbells per launch) and the GPU's
// on-chip vs DRAM memory hierarchy:
//
//   * "staged"  — one bit-reversal kernel + log2(N) butterfly-stage kernels,
//                 each a separate launch operating in global memory.  Maximizes
//                 host<->GPU command traffic (1 + log2(N) launches).
//   * "shared"  — a single block that loads N points into shared memory and
//                 runs every stage on-chip with one launch.
//
// Twiddle factors are precomputed on the host and passed via an H2D buffer so
// the kernels contain no transcendental math: the result is deterministic and
// the exact test vectors (impulse, DC) produce bit-exact float output.
//
// Decimation-in-time, single-precision complex (float2: .x=real, .y=imag).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef FFT_PI
#define FFT_PI 3.14159265358979323846
#endif

// Reverse the low `logn` bits of x (bit-reversal permutation index).
__device__ __forceinline__ unsigned fft_bitrev_index(unsigned x, int logn)
{
    unsigned r = 0;
    for (int i = 0; i < logn; i++) {
        r = (r << 1) | (x & 1u);
        x >>= 1;
    }
    return r;
}

__device__ __forceinline__ float2 fft_cmul(float2 a, float2 b)
{
    float2 r;
    r.x = a.x * b.x - a.y * b.y;
    r.y = a.x * b.y + a.y * b.x;
    return r;
}

// Stage 1 of the staged path: scatter inputs into bit-reversed order.
//   out[i] = in[bitrev(i)]
__global__ void fft_bitrev(float2 *out, const float2 *in, int n, int logn)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n)
        out[i] = in[fft_bitrev_index((unsigned)i, logn)];
}

// One in-place radix-2 butterfly stage (s in 1..logn), N/2 butterflies total.
// half = 2^(s-1), groups of m = 2^s.  Twiddle for butterfly j in a group is
// tw[j << (logn - s)] = W_N^{j * N/m}.  Each thread owns a disjoint (i0,i1)
// pair, so the in-place update is safe across all threads in the launch.
__global__ void fft_stage(float2 *data, const float2 *tw, int n, int s, int logn)
{
    int k = blockIdx.x * blockDim.x + threadIdx.x;
    if (k >= (n >> 1))
        return;

    int half = 1 << (s - 1);
    int j = k & (half - 1);
    int group = k >> (s - 1);
    int i0 = group * (half << 1) + j;
    int i1 = i0 + half;

    float2 w = tw[j << (logn - s)];
    float2 a = data[i0];
    float2 b = data[i1];
    float2 t = fft_cmul(w, b);

    data[i0].x = a.x + t.x;
    data[i0].y = a.y + t.y;
    data[i1].x = a.x - t.x;
    data[i1].y = a.y - t.y;
}

// Single-block, all-stages shared-memory FFT (the A/B contrast variant).
// Requires n <= 2 * blockDim.x and n * sizeof(float2) bytes of shared memory.
extern __shared__ float2 fft_smem[];
__global__ void fft_shared(float2 *data, const float2 *tw, int n, int logn)
{
    int tid = threadIdx.x;

    // Load in bit-reversed order straight into shared memory.
    for (int i = tid; i < n; i += blockDim.x)
        fft_smem[i] = data[fft_bitrev_index((unsigned)i, logn)];
    __syncthreads();

    for (int s = 1; s <= logn; s++) {
        int half = 1 << (s - 1);
        for (int k = tid; k < (n >> 1); k += blockDim.x) {
            int j = k & (half - 1);
            int group = k >> (s - 1);
            int i0 = group * (half << 1) + j;
            int i1 = i0 + half;
            float2 w = tw[j << (logn - s)];
            float2 a = fft_smem[i0];
            float2 b = fft_smem[i1];
            float2 t = fft_cmul(w, b);
            fft_smem[i0].x = a.x + t.x;
            fft_smem[i0].y = a.y + t.y;
            fft_smem[i1].x = a.x - t.x;
            fft_smem[i1].y = a.y - t.y;
        }
        __syncthreads();
    }

    for (int i = tid; i < n; i += blockDim.x)
        data[i] = fft_smem[i];
}

static int ilog2(int n)
{
    int l = 0;
    while ((1 << l) < n)
        l++;
    return l;
}

int main(int argc, char *argv[])
{
    // Args: fft [N] [staged|shared].  N must be a power of two.
    int n = (argc > 1) ? atoi(argv[1]) : 256;
    const char *mode = (argc > 2) ? argv[2] : "staged";
    int logn = ilog2(n);
    if ((1 << logn) != n) {
        fprintf(stderr, "N must be a power of two (got %d)\n", n);
        return 2;
    }

    size_t vec_bytes = (size_t)n * sizeof(float2);
    size_t tw_bytes = (size_t)(n / 2) * sizeof(float2);

    float2 *h_in = (float2 *)malloc(vec_bytes);
    float2 *h_out = (float2 *)malloc(vec_bytes);
    float2 *h_tw = (float2 *)malloc(tw_bytes);

    // Exact test vector: unit impulse x[n] = delta[n]  ->  X[k] = 1+0j for all k.
    for (int i = 0; i < n; i++) {
        h_in[i].x = (i == 0) ? 1.0f : 0.0f;
        h_in[i].y = 0.0f;
    }
    // Precompute twiddles W_N^t = exp(-2*pi*i*t/N), t = 0 .. N/2-1.
    for (int t = 0; t < n / 2; t++) {
        h_tw[t].x = (float)cos(-2.0 * FFT_PI * (double)t / (double)n);
        h_tw[t].y = (float)sin(-2.0 * FFT_PI * (double)t / (double)n);
    }

    float2 *d_a, *d_b, *d_tw;
    cudaMalloc(&d_a, vec_bytes);
    cudaMalloc(&d_b, vec_bytes);
    cudaMalloc(&d_tw, tw_bytes);

    cudaMemcpy(d_a, h_in, vec_bytes, cudaMemcpyHostToDevice);
    cudaMemcpy(d_tw, h_tw, tw_bytes, cudaMemcpyHostToDevice);

    int blockSize = 256;
    if (blockSize > n)
        blockSize = n;

    if (strcmp(mode, "shared") == 0) {
        // One launch, all stages on-chip.
        int threads = n / 2;
        if (threads > 1024)
            threads = 1024;
        fft_shared<<<1, threads, vec_bytes>>>(d_a, d_tw, n, logn);
        cudaMemcpy(h_out, d_a, vec_bytes, cudaMemcpyDeviceToHost);
    } else {
        // Staged: bit-reversal then log2(N) in-place butterfly launches.
        int bitrevGrid = (n + blockSize - 1) / blockSize;
        int bflyThreads = n / 2;
        int bflyGrid = (bflyThreads + blockSize - 1) / blockSize;
        fft_bitrev<<<bitrevGrid, blockSize>>>(d_b, d_a, n, logn);
        for (int s = 1; s <= logn; s++)
            fft_stage<<<bflyGrid, blockSize>>>(d_b, d_tw, n, s, logn);
        cudaMemcpy(h_out, d_b, vec_bytes, cudaMemcpyDeviceToHost);
    }

    // Verify the impulse invariant: every bin must be exactly 1 + 0j.
    int errors = 0;
    for (int k = 0; k < n; k++) {
        if (h_out[k].x != 1.0f || h_out[k].y != 0.0f)
            errors++;
    }
    printf("FFT N=%d mode=%s launches=%d : impulse check %s (errors=%d)\n",
           n, mode, (strcmp(mode, "shared") == 0) ? 1 : (1 + logn),
           errors == 0 ? "PASS" : "FAIL", errors);

    cudaFree(d_a);
    cudaFree(d_b);
    cudaFree(d_tw);
    free(h_in);
    free(h_out);
    free(h_tw);
    return errors == 0 ? 0 : 1;
}
