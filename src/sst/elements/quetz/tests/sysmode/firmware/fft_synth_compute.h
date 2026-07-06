/*
 * fft_synth_compute.h — CPU-side staged radix-2 FFT (bit-reversal + log2(N)
 * butterflies) shared by the RISC-V and m68k synthetic-GPU demos; a port of
 * balar_trace/fft_reference.py. Scalar type is hardware float32 by default;
 * -DFFT_FIXED_POINT uses Q16.16 on ColdFire V2 because its m68k libgcc soft-float
 * (__mulsf3 &c.) HANGS. Impulse input -> X[k]=1+0j, bit-exact (twiddles multiply
 * only zero); verify_impulse() counts exactly-correct scalar words.
 */

#ifndef FFT_SYNTH_COMPUTE_H
#define FFT_SYNTH_COMPUTE_H

#include <stdint.h>

#include "fft_firmware_data_256.h"   /* FFT_N, FFT_LOGN, fft_tw_bits[] */

#ifdef FFT_FIXED_POINT

/* ---- Q16.16 fixed-point scalar (integer-only; for the FPU-less ColdFire) ---- */
typedef int32_t fft_scalar;
#define FFT_ONE   ((fft_scalar)0x00010000)   /* 1.0 in Q16.16 */
#define FFT_ZERO  ((fft_scalar)0)

/* Q16.16 multiply, computed with only 32-bit multiplies (no int64 — the FPU-less
 * ColdFire would lower a 64-bit multiply to libgcc's __muldi3, which HANGS on
 * ColdFire V2). We form the unsigned 64-bit product as hi:lo two 32-bit words
 * from 16-bit-limb partials (each a 32-bit ColdFire mulu.l), take bits [47:16]
 * as the Q16.16 magnitude, then apply the sign. Result matches
 * ((int64_t)a*b)>>16 for the bounded FFT range (|value| < 2^24). */
static inline fft_scalar fft_mul(fft_scalar a, fft_scalar b)
{
    int32_t neg = 0;
    uint32_t ua = (a < 0) ? (neg ^= 1, (uint32_t)(-a)) : (uint32_t)a;
    uint32_t ub = (b < 0) ? (neg ^= 1, (uint32_t)(-b)) : (uint32_t)b;

    uint32_t a0 = ua & 0xFFFFu, a1 = ua >> 16;   /* 16-bit limbs */
    uint32_t b0 = ub & 0xFFFFu, b1 = ub >> 16;

    /* 64-bit product = lo (bits[31:0]) + hi (bits[63:32]), carry-correct. */
    uint32_t p00 = a0 * b0;                 /* bits [31:0]  */
    uint32_t p01 = a0 * b1;                 /* bits [47:16] */
    uint32_t p10 = a1 * b0;                 /* bits [47:16] */
    uint32_t p11 = a1 * b1;                 /* bits [63:32] */

    uint32_t mid = (p00 >> 16) + (p01 & 0xFFFFu) + (p10 & 0xFFFFu);
    uint32_t lo  = (p00 & 0xFFFFu) | (mid << 16);
    uint32_t hi  = p11 + (p01 >> 16) + (p10 >> 16) + (mid >> 16);

    /* magnitude >> 16 = low 16 bits of hi joined to high 16 bits of lo */
    uint32_t mag = (lo >> 16) | (hi << 16);
    return neg ? -(int32_t)mag : (int32_t)mag;
}
static inline fft_scalar fft_add(fft_scalar a, fft_scalar b) { return a + b; }
static inline fft_scalar fft_sub(fft_scalar a, fft_scalar b) { return a - b; }

/* IEEE-754 float32 bits -> Q16.16 (used only to load the twiddle table). */
static inline fft_scalar fft_bits_to_scalar(uint32_t bits)
{
    uint32_t mant = (bits & 0x007FFFFFu) | 0x00800000u;  /* implicit 1 */
    int32_t  exp  = (int32_t)((bits >> 23) & 0xFFu) - 127;
    int32_t  sign = (bits & 0x80000000u) ? -1 : 1;
    if (((bits >> 23) & 0xFFu) == 0u)                    /* zero/subnormal -> 0 */
        return 0;
    /* value = mant * 2^(exp-23); want Q16.16 => shift by (exp-23+16). */
    int32_t sh = exp - 23 + 16;
    int64_t m  = (int64_t)mant;
    int64_t q  = (sh >= 0) ? (m << sh) : (m >> (-sh));
    return (fft_scalar)(sign * (int32_t)q);
}

#else

/* ---- float32 scalar (default; RISC-V rv64gc has hardware FP) ---------------- */
typedef float fft_scalar;
#define FFT_ONE   (1.0f)
#define FFT_ZERO  (0.0f)

static inline fft_scalar fft_mul(fft_scalar a, fft_scalar b) { return a * b; }
static inline fft_scalar fft_add(fft_scalar a, fft_scalar b) { return a + b; }
static inline fft_scalar fft_sub(fft_scalar a, fft_scalar b) { return a - b; }

static inline fft_scalar fft_bits_to_scalar(uint32_t bits)
{
    float f;
    __builtin_memcpy(&f, &bits, sizeof(f));
    return f;
}

#endif  /* FFT_FIXED_POINT */

typedef struct { fft_scalar re, im; } cfloat;

/* fft_tw_bits[] is interleaved (re,im) float32 bits; expose it as cfloat tw[N/2]
 * in the active scalar representation. */
static inline cfloat fft_tw(uint32_t t)
{
    cfloat w;
    w.re = fft_bits_to_scalar(fft_tw_bits[2u * t + 0u]);
    w.im = fft_bits_to_scalar(fft_tw_bits[2u * t + 1u]);
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
        t.re = fft_sub(fft_mul(w.re, ai1.re), fft_mul(w.im, ai1.im));
        t.im = fft_add(fft_mul(w.re, ai1.im), fft_mul(w.im, ai1.re));
        cfloat   ai0   = a[i0];
        a[i0].re = fft_add(ai0.re, t.re);  a[i0].im = fft_add(ai0.im, t.im);
        a[i1].re = fft_sub(ai0.re, t.re);  a[i1].im = fft_sub(ai0.im, t.im);
    }
}

/* Impulse verification: X[k] must be exactly 1+0j for all k. Returns the count of
 * exactly-correct scalar words (2 per complex point => 2*N total). */
static inline uint32_t fft_verify_impulse(const cfloat *out, uint32_t n)
{
    uint32_t correct = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (out[i].re == FFT_ONE)  correct++;
        if (out[i].im == FFT_ZERO) correct++;
    }
    return correct;
}

#endif /* FFT_SYNTH_COMPUTE_H */
