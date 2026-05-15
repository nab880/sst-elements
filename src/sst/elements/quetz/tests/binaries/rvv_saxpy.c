/**
 * rvv_saxpy.c — standalone SAXPY benchmark using RISC-V Vector (RVV 1.0).
 *
 * Computes:  Y[i] = a * X[i] + Y[i]  for i = 0..N-1
 * using RVV intrinsics with a strip-mining loop that works for any N.
 *
 * Deliberately no-libc: uses only direct syscalls so all data accesses
 * stay in the statically-mapped low-address region (< a few MB), well
 * within the SST-simulated memory range.
 *
 * Build:
 *   /opt/riscv/bin/riscv64-unknown-linux-musl-gcc \
 *     -march=rv64gcv -mabi=lp64d -O2 -static \
 *     -nostdlib -ffreestanding \
 *     -e _start -Wl,--build-id=none \
 *     rvv_saxpy.c -o rvv_saxpy
 */

#include <riscv_vector.h>
#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Problem dimensions.
 * N=64 elements → 256 bytes each for X/Y → fits in 1 cache line set.
 * With VLEN=128, each vfmv/vfmacc processes 4 float32 elements at a time
 * (LMUL=1 → 128/32 = 4 elements per register group).
 * -------------------------------------------------------------------------*/
#define N 64

/* Static storage — lives in .bss/.data, loaded at a low VA by the kernel. */
static float X[N];
static float Y[N];
static float Z[N];   /* reference result for scalar verification */

/* -------------------------------------------------------------------------
 * Direct syscall wrappers (no libc).
 * -------------------------------------------------------------------------*/
static inline long sys_write(int fd, const void *buf, size_t n) {
    register long a7 __asm__("a7") = 64;  /* SYS_write */
    register long a0 __asm__("a0") = fd;
    register long a1 __asm__("a1") = (long)buf;
    register long a2 __asm__("a2") = (long)n;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2) : "memory");
    return a0;
}

static inline void sys_exit(int code) {
    register long a7 __asm__("a7") = 93;  /* SYS_exit */
    register long a0 __asm__("a0") = code;
    __asm__ volatile("ecall" :: "r"(a7), "r"(a0) : "memory");
    __builtin_unreachable();
}

/* memset is needed by GCC for partial-init of local arrays (-ffreestanding
 * still emits calls to it).  Provide a minimal version rather than
 * bringing in libc. */
void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

/* Minimal itoa for pass/fail reporting. */
static int itoa_buf(long v, char *buf) {
    if (v == 0) { buf[0]='0'; return 1; }
    char tmp[24]; int n=0;
    int neg = (v < 0); if (neg) v = -v;
    while (v) { tmp[n++] = '0' + (v % 10); v /= 10; }
    if (neg) tmp[n++] = '-';
    int i=0;
    for (int j=n-1; j>=0; j--) buf[i++] = tmp[j];
    return i;
}

static void print(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    sys_write(1, s, n);
}

/* -------------------------------------------------------------------------
 * RVV SAXPY kernel:  Y[i] += a * X[i]
 * Uses vfmacc (fused multiply-accumulate) for accuracy and throughput.
 * -------------------------------------------------------------------------*/
static void saxpy_rvv(float a, const float * restrict x,
                      float * restrict y, size_t n) {
    for (size_t vl; n > 0; n -= vl, x += vl, y += vl) {
        vl = __riscv_vsetvl_e32m2(n);               /* LMUL=2 → 8 elem/group @VLEN=128 */
        vfloat32m2_t vx = __riscv_vle32_v_f32m2(x, vl);   /* vector load  */
        vfloat32m2_t vy = __riscv_vle32_v_f32m2(y, vl);   /* vector load  */
        vy = __riscv_vfmacc_vf_f32m2(vy, a, vx, vl);      /* y += a * x   */
        __riscv_vse32_v_f32m2(y, vy, vl);                  /* vector store */
    }
}

/* -------------------------------------------------------------------------
 * Scalar SAXPY for reference.
 * -------------------------------------------------------------------------*/
static void saxpy_scalar(float a, const float * restrict x,
                         float * restrict y, size_t n) {
    for (size_t i = 0; i < n; i++)
        y[i] += a * x[i];
}

/* Simple approximate float compare (no libm, no memcpy). */
static int approx_equal(float a, float b) {
    /* reinterpret as int32 via union to avoid memcpy call */
    union { float f; uint32_t u; } ua = { a }, ub = { b };
    long diff = (long)(ua.u > ub.u ? ua.u - ub.u : ub.u - ua.u);
    return diff < 2;   /* allow 1-ULP difference */
}

/* -------------------------------------------------------------------------
 * Entry point (no libc, so we define _start ourselves).
 * -------------------------------------------------------------------------*/
void __attribute__((noreturn)) _start(void) {
    const float a = 2.5f;

    /* Initialise arrays with recognisable values. */
    for (int i = 0; i < N; i++) {
        X[i] = (float)(i + 1);        /* 1, 2, 3, … 64  */
        Y[i] = (float)(N - i);        /* 64, 63, … 1    */
        Z[i] = Y[i];                  /* reference copy  */
    }

    /* Scalar reference. */
    saxpy_scalar(a, X, Z, N);

    /* RVV kernel. */
    saxpy_rvv(a, X, Y, N);

    /* Verify. */
    int errors = 0;
    for (int i = 0; i < N; i++) {
        if (!approx_equal(Y[i], Z[i]))
            errors++;
    }

    if (errors == 0) {
        print("PASS: saxpy_rvv correct for N=64\n");
        sys_exit(0);
    } else {
        char msg[64] = "FAIL: ";
        int pos = 6;
        pos += itoa_buf(errors, msg + pos);
        msg[pos++] = ' '; msg[pos++] = 'e';
        msg[pos++] = 'r'; msg[pos++] = 'r';
        msg[pos++] = 's'; msg[pos++] = '\n';
        sys_write(1, msg, pos);
        sys_exit(1);
    }
}
