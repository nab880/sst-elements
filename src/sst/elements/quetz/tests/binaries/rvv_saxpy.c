

#include <riscv_vector.h>
#include <stddef.h>
#include <stdint.h>


#define N 64


static float X[N];
static float Y[N];
static float Z[N];   


static inline long sys_write(int fd, const void *buf, size_t n) {
    register long a7 __asm__("a7") = 64;  
    register long a0 __asm__("a0") = fd;
    register long a1 __asm__("a1") = (long)buf;
    register long a2 __asm__("a2") = (long)n;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2) : "memory");
    return a0;
}

static inline void sys_exit(int code) {
    register long a7 __asm__("a7") = 93;  
    register long a0 __asm__("a0") = code;
    __asm__ volatile("ecall" :: "r"(a7), "r"(a0) : "memory");
    __builtin_unreachable();
}


void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}


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


static void saxpy_rvv(float a, const float * restrict x,
                      float * restrict y, size_t n) {
    for (size_t vl; n > 0; n -= vl, x += vl, y += vl) {
        vl = __riscv_vsetvl_e32m2(n);               
        vfloat32m2_t vx = __riscv_vle32_v_f32m2(x, vl);   
        vfloat32m2_t vy = __riscv_vle32_v_f32m2(y, vl);   
        vy = __riscv_vfmacc_vf_f32m2(vy, a, vx, vl);      
        __riscv_vse32_v_f32m2(y, vy, vl);                  
    }
}


static void saxpy_scalar(float a, const float * restrict x,
                         float * restrict y, size_t n) {
    for (size_t i = 0; i < n; i++)
        y[i] += a * x[i];
}


static int approx_equal(float a, float b) {
    
    union { float f; uint32_t u; } ua = { a }, ub = { b };
    long diff = (long)(ua.u > ub.u ? ua.u - ub.u : ub.u - ua.u);
    return diff < 2;   
}


void __attribute__((noreturn)) _start(void) {
    const float a = 2.5f;

    
    for (int i = 0; i < N; i++) {
        X[i] = (float)(i + 1);        
        Y[i] = (float)(N - i);        
        Z[i] = Y[i];                  
    }

    
    saxpy_scalar(a, X, Z, N);

    
    saxpy_rvv(a, X, Y, N);

    
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
