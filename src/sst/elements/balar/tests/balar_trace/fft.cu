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
// Implementations (selected by main()'s mode arg):
//   "staged"    — bit-reversal kernel + log2(N) butterfly-stage kernels in
//                 global memory (max host<->GPU command traffic).
//   "shared"    — a single block running every stage on-chip (A/B contrast).
//   "roundtrip" — forward FFT then inverse FFT (conjugated twiddles + 1/N);
//                 reports IFFT(FFT(x)) recovery error. This exercises every
//                 twiddle and is a self-consistent accuracy check needing no
//                 external reference.
//
// Precision is compile-time selectable: -DFFT_DOUBLE -> double2 (~1e-16),
// default float2 (~1e-7). The kernels, twiddle table, and trace ABI follow it.
//
// Twiddles are precomputed on the host so the kernels carry no transcendental
// math: the result is deterministic.  Decimation-in-time, complex.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef FFT_PI
#define FFT_PI 3.14159265358979323846
#endif

#ifdef FFT_DOUBLE
typedef double  fft_real;
typedef double2 fft_cplx;
#else
typedef float   fft_real;
typedef float2  fft_cplx;
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

__device__ __forceinline__ fft_cplx fft_cmul(fft_cplx a, fft_cplx b)
{
    fft_cplx r;
    r.x = a.x * b.x - a.y * b.y;
    r.y = a.x * b.y + a.y * b.x;
    return r;
}

// Stage 1 of the staged path: scatter inputs into bit-reversed order.
//   out[i] = in[bitrev(i)]
__global__ void fft_bitrev(fft_cplx *out, const fft_cplx *in, int n, int logn)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n)
        out[i] = in[fft_bitrev_index((unsigned)i, logn)];
}

// One in-place radix-2 butterfly stage (s in 1..logn), N/2 butterflies total.
// half = 2^(s-1), groups of m = 2^s.  Twiddle for butterfly j in a group is
// tw[j << (logn - s)] = W_N^{j * N/m}.  Each thread owns a disjoint (i0,i1)
// pair, so the in-place update is safe across all threads in the launch.
// Passing a conjugated twiddle table turns this into an inverse-transform stage.
__global__ void fft_stage(fft_cplx *data, const fft_cplx *tw, int n, int s, int logn)
{
    int k = blockIdx.x * blockDim.x + threadIdx.x;
    if (k >= (n >> 1))
        return;

    int half = 1 << (s - 1);
    int j = k & (half - 1);
    int group = k >> (s - 1);
    int i0 = group * (half << 1) + j;
    int i1 = i0 + half;

    fft_cplx w = tw[j << (logn - s)];
    fft_cplx a = data[i0];
    fft_cplx b = data[i1];
    fft_cplx t = fft_cmul(w, b);

    data[i0].x = a.x + t.x;
    data[i0].y = a.y + t.y;
    data[i1].x = a.x - t.x;
    data[i1].y = a.y - t.y;
}

// Scale every element by `s` (used by the inverse transform: s = 1/N).
__global__ void fft_scale(fft_cplx *data, int n, fft_real s)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        data[i].x *= s;
        data[i].y *= s;
    }
}

// Single-block, all-stages shared-memory FFT (the A/B contrast variant).
// Strided loops handle any N that fits in shared memory (N*sizeof(fft_cplx)
// bytes); it is not limited to N <= 2*blockDim.
extern __shared__ fft_cplx fft_smem[];
__global__ void fft_shared(fft_cplx *data, const fft_cplx *tw, int n, int logn)
{
    int tid = threadIdx.x;

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
            fft_cplx w = tw[j << (logn - s)];
            fft_cplx a = fft_smem[i0];
            fft_cplx b = fft_smem[i1];
            fft_cplx t = fft_cmul(w, b);
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

// W_N^t = exp(-2*pi*i*t/N), t = 0 .. N/2-1.  conj=1 builds the inverse table.
// Computed in double then narrowed to fft_real; -0.0 normalized to +0.0.
static void make_twiddles(fft_cplx *tw, int n, int conj)
{
    for (int t = 0; t < n / 2; t++) {
        double ang = -2.0 * FFT_PI * (double)t / (double)n;
        fft_real re = (fft_real)cos(ang);
        fft_real im = (fft_real)sin(ang);
        if (im == (fft_real)0)
            im = (fft_real)0;          // normalize -0.0 -> +0.0
        tw[t].x = re;
        tw[t].y = conj ? -im : im;
    }
}

#define SHARED_BYTES_MAX (48 * 1024)

int main(int argc, char *argv[])
{
    // Args: fft [N] [staged|shared|roundtrip]. N must be a power of two.
    int n = (argc > 1) ? atoi(argv[1]) : 256;
    const char *mode = (argc > 2) ? argv[2] : "staged";
    int logn = ilog2(n);
    if ((1 << logn) != n) {
        fprintf(stderr, "N must be a power of two (got %d)\n", n);
        return 2;
    }
    int is_rt = (strcmp(mode, "roundtrip") == 0);
    int is_shared = (strcmp(mode, "shared") == 0);
    const int k0 = 5;   // tone bin for roundtrip

    size_t vec_bytes = (size_t)n * sizeof(fft_cplx);
    size_t tw_bytes = (size_t)(n / 2) * sizeof(fft_cplx);

    fft_cplx *h_in = (fft_cplx *)malloc(vec_bytes);
    fft_cplx *h_out = (fft_cplx *)malloc(vec_bytes);
    fft_cplx *h_spec = (fft_cplx *)malloc(vec_bytes);
    fft_cplx *h_tw = (fft_cplx *)malloc(tw_bytes);
    fft_cplx *h_twc = (fft_cplx *)malloc(tw_bytes);

    for (int i = 0; i < n; i++) {
        // roundtrip uses a tone (exercises all twiddles); staged/shared use the
        // bit-exact impulse vector that the trace tests gate on.
        h_in[i].x = is_rt ? (fft_real)cos(2.0 * FFT_PI * k0 * i / n)
                          : (fft_real)((i == 0) ? 1.0 : 0.0);
        h_in[i].y = (fft_real)0.0;
    }
    make_twiddles(h_tw, n, 0);
    make_twiddles(h_twc, n, 1);

    fft_cplx *d_a, *d_b, *d_tw;
    cudaMalloc(&d_a, vec_bytes);
    cudaMalloc(&d_b, vec_bytes);
    cudaMalloc(&d_tw, tw_bytes);
    cudaMemcpy(d_a, h_in, vec_bytes, cudaMemcpyHostToDevice);
    cudaMemcpy(d_tw, h_tw, tw_bytes, cudaMemcpyHostToDevice);

    int blockSize = 256;
    if (blockSize > n)
        blockSize = n;
    int bitrevGrid = (n + blockSize - 1) / blockSize;
    int bflyGrid = ((n / 2) + blockSize - 1) / blockSize;

    if (is_shared) {
        int threads = n / 2;
        if (threads > 1024)
            threads = 1024;
        if (vec_bytes > SHARED_BYTES_MAX) {
            fprintf(stderr, "shared mode needs %zu B shared mem (> %d); use staged\n",
                    vec_bytes, SHARED_BYTES_MAX);
            return 2;
        }
        fft_shared<<<1, threads, vec_bytes>>>(d_a, d_tw, n, logn);
        cudaMemcpy(h_out, d_a, vec_bytes, cudaMemcpyDeviceToHost);
    } else if (is_rt) {
        fft_cplx *d_c, *d_twc;
        cudaMalloc(&d_c, vec_bytes);
        cudaMalloc(&d_twc, tw_bytes);
        cudaMemcpy(d_twc, h_twc, tw_bytes, cudaMemcpyHostToDevice);
        // Forward FFT: d_a -> d_b.
        fft_bitrev<<<bitrevGrid, blockSize>>>(d_b, d_a, n, logn);
        for (int s = 1; s <= logn; s++)
            fft_stage<<<bflyGrid, blockSize>>>(d_b, d_tw, n, s, logn);
        cudaMemcpy(h_spec, d_b, vec_bytes, cudaMemcpyDeviceToHost);
        // Inverse FFT (conjugated twiddles + 1/N): d_b -> d_c.
        fft_bitrev<<<bitrevGrid, blockSize>>>(d_c, d_b, n, logn);
        for (int s = 1; s <= logn; s++)
            fft_stage<<<bflyGrid, blockSize>>>(d_c, d_twc, n, s, logn);
        fft_scale<<<bitrevGrid, blockSize>>>(d_c, n, (fft_real)1.0 / (fft_real)n);
        cudaMemcpy(h_out, d_c, vec_bytes, cudaMemcpyDeviceToHost);
        cudaFree(d_c);
        cudaFree(d_twc);
    } else { // staged
        fft_bitrev<<<bitrevGrid, blockSize>>>(d_b, d_a, n, logn);
        for (int s = 1; s <= logn; s++)
            fft_stage<<<bflyGrid, blockSize>>>(d_b, d_tw, n, s, logn);
        cudaMemcpy(h_out, d_b, vec_bytes, cudaMemcpyDeviceToHost);
    }

    int rc = 0;
    if (is_rt) {
        // Report the dominant spectral bins (should peak at k0 and N-k0) and the
        // round-trip recovery error.
        int p0 = 0;
        double best = -1.0;
        for (int k = 0; k < n; k++) {
            double mag = (double)h_spec[k].x * h_spec[k].x + (double)h_spec[k].y * h_spec[k].y;
            if (mag > best) { best = mag; p0 = k; }
        }
        double err = 0.0, ref = 0.0;
        for (int i = 0; i < n; i++) {
            double dr = (double)h_out[i].x - (double)h_in[i].x;
            double di = (double)h_out[i].y - (double)h_in[i].y;
            err += dr * dr + di * di;
            ref += (double)h_in[i].x * h_in[i].x + (double)h_in[i].y * h_in[i].y;
        }
        double rel = sqrt(err) / (sqrt(ref) > 0.0 ? sqrt(ref) : 1.0);
        printf("FFT N=%d mode=roundtrip prec=%s : spectral peak bin=%d (tone k0=%d), "
               "IFFT(FFT(x)) rel-err=%.3e %s\n",
               n, (sizeof(fft_real) == 8) ? "double" : "float", p0, k0, rel,
               rel < 1e-3 ? "PASS" : "FAIL");
        rc = (p0 == k0 || p0 == n - k0) && rel < 1e-3 ? 0 : 1;
    } else {
        int errors = 0;
        for (int k = 0; k < n; k++)
            if (h_out[k].x != (fft_real)1.0 || h_out[k].y != (fft_real)0.0)
                errors++;
        printf("FFT N=%d mode=%s launches=%d : impulse check %s (errors=%d)\n",
               n, mode, is_shared ? 1 : (1 + logn),
               errors == 0 ? "PASS" : "FAIL", errors);
        rc = errors == 0 ? 0 : 1;
    }

    cudaFree(d_a);
    cudaFree(d_b);
    cudaFree(d_tw);
    free(h_in);
    free(h_out);
    free(h_spec);
    free(h_tw);
    free(h_twc);
    return rc;
}
