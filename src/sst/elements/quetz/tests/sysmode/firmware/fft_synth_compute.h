/*
 * fft_synth_compute.h — CPU-side staged radix-2 FFT for the synthetic-GPU demo.
 *
 * The synthetic QuetzGpuDevice is a pure latency model (doorbell -> BUSY -> IDLE,
 * no compute, no DMA). So the FFT math runs here on the guest CPU, while each
 * "kernel" is *timed* by ringing the device doorbell. This header holds only the
 * arithmetic (bit-reversal + log2(N) butterfly stages) so both the RISC-V and
 * m68k firmwares share one algorithm; each firmware owns its own MMIO/UART and
 * the launch_timed() doorbell loop.
 *
 * It is a direct port of balar/tests/balar_trace/fft_reference.py:fft_staged()
 * (the host twin of fft.cu). No libm, no balar, no wire packet. The twiddle table
 * comes from fft_firmware_data_256.h (fft_tw_bits[], IEEE-754 float32 bits).
 *
 * Impulse input (x[0]=1, rest 0) -> X[k] = 1+0j for all k. The twiddles only ever
 * multiply zero in that case, so the result is BIT-EXACT vs 1.0f with no
 * tolerance — verify_impulse() counts exactly-correct words (512 for N=256).
 */

#ifndef FFT_SYNTH_COMPUTE_H
#define FFT_SYNTH_COMPUTE_H

#include <stdint.h>

#include "fft_firmware_data_256.h"   /* FFT_N, FFT_LOGN, fft_tw_bits[] */

typedef struct { float re, im; } cfloat;

/* IEEE-754 float32 bit pattern -> float, no strict-aliasing UB (memcpy-style). */
static inline float fft_bits_to_f32(uint32_t bits)
{
    float f;
    __builtin_memcpy(&f, &bits, sizeof(f));
    return f;
}

/* fft_tw_bits[] is interleaved (re,im) float32 bits; expose it as cfloat tw[N/2]. */
static inline cfloat fft_tw(uint32_t t)
{
    cfloat w;
    w.re = fft_bits_to_f32(fft_tw_bits[2u * t + 0u]);
    w.im = fft_bits_to_f32(fft_tw_bits[2u * t + 1u]);
    return w;
}

/* bitrev(x, logn) — identical to fft_reference.py:bitrev(). */
static inline uint32_t fft_bitrev(uint32_t x, uint32_t logn)
{
    uint32_t r = 0;
    for (uint32_t i = 0; i < logn; i++) {
        r = (r << 1) | (x & 1u);
        x >>= 1;
    }
    return r;
}

/* Bit-reversal permutation: out[i] = in[bitrev(i)]. (fft.cu's fft_bitrev kernel.) */
static inline void fft_permute_bitrev(cfloat *out, const cfloat *in,
                                      uint32_t n, uint32_t logn)
{
    for (uint32_t i = 0; i < n; i++)
        out[i] = in[fft_bitrev(i, logn)];
}

/* One in-place butterfly stage s (1..logn). Mirror of fft_reference.py:fft_staged
 * inner loop / fft.cu's fft_stage kernel. */
static inline void fft_stage(cfloat *a, uint32_t n, uint32_t s, uint32_t logn)
{
    uint32_t half = 1u << (s - 1u);
    for (uint32_t k = 0; k < n / 2u; k++) {
        uint32_t j     = k & (half - 1u);
        uint32_t group = k >> (s - 1u);
        uint32_t i0    = group * (half << 1u) + j;
        uint32_t i1    = i0 + half;
        cfloat   w     = fft_tw(j << (logn - s));
        cfloat   ai1   = a[i1];
        /* t = w * a[i1] (complex multiply) */
        cfloat   t;
        t.re = w.re * ai1.re - w.im * ai1.im;
        t.im = w.re * ai1.im + w.im * ai1.re;
        cfloat   ai0   = a[i0];
        a[i0].re = ai0.re + t.re;  a[i0].im = ai0.im + t.im;
        a[i1].re = ai0.re - t.re;  a[i1].im = ai0.im - t.im;
    }
}

/* Impulse verification: X[k] must be exactly 1+0j for all k. Returns the count of
 * exactly-correct float words (2 per complex point => 2*N total). */
static inline uint32_t fft_verify_impulse(const cfloat *out, uint32_t n)
{
    uint32_t correct = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (out[i].re == 1.0f) correct++;
        if (out[i].im == 0.0f) correct++;
    }
    return correct;
}

#endif /* FFT_SYNTH_COMPUTE_H */
