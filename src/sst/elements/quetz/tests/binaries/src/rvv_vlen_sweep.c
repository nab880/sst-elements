/* rvv_vlen_sweep — SAXPY with RVV; varies effective vector length. */
#include <riscv_vector.h>
#include <stdint.h>
#include <stdio.h>

#define N 128
static float X[N];
static float Y[N];

int main(void) {
    for (int i = 0; i < N; i++) {
        X[i] = (float)i;
        Y[i] = 1.0f;
    }
    const float a = 2.0f;
    size_t vl;
    for (int i = 0; i < N;) {
        vl = __riscv_vsetvl_e32m1((size_t)(N - i));
        vfloat32m1_t vx = __riscv_vle32_v_f32m1(&X[i], vl);
        vfloat32m1_t vy = __riscv_vle32_v_f32m1(&Y[i], vl);
        vy = __riscv_vfmacc_vf_f32m1(vy, a, vx, vl);
        __riscv_vse32_v_f32m1(&Y[i], vy, vl);
        i += (int)vl;
    }
    printf("rvv_vlen_sweep Y[0]=%f\n", Y[0]);
    return 0;
}
