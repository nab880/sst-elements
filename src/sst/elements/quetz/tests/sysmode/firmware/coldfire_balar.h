

#ifndef COLDFIRE_BALAR_H
#define COLDFIRE_BALAR_H

#include <stdint.h>
#include <stddef.h>


#define BALAR_DOORBELL 0x70000000UL


#define BP_SIZE            536u   
#define BP_CALL_ID          0u    
#define BP_ISSSTMEM         4u    
#define BP_UNION            8u

#define BP_MALLOC_DEVPTR    8u
#define BP_MALLOC_SIZE      16u

#define BP_FATBIN_NAME      8u

#define BP_FUNC_HANDLE      8u
#define BP_FUNC_HOSTFUN     16u
#define BP_FUNC_DEVICEFUN   24u

#define BP_MEMCPY_DST       8u
#define BP_MEMCPY_SRC       16u
#define BP_MEMCPY_COUNT     24u
#define BP_MEMCPY_PAYLOAD   32u
#define BP_MEMCPY_KIND      56u

#define BP_CFG_SHAREDMEM    8u
#define BP_CFG_GDX          24u
#define BP_CFG_GDY          28u
#define BP_CFG_GDZ          32u
#define BP_CFG_BDX          36u
#define BP_CFG_BDY          40u
#define BP_CFG_BDZ          44u

#define BP_ARG_ARG          8u
#define BP_ARG_SIZE         16u
#define BP_ARG_OFFSET       24u
#define BP_ARG_VALUE        32u

#define BP_LAUNCH_FUNC      8u
#define BP_FREE_DEVPTR      8u


#define CB_REG_FAT_BINARY   1
#define CB_REG_FUNCTION     2
#define CB_MEMCPY           3
#define CB_CONFIG_CALL      4
#define CB_SET_ARG          5
#define CB_LAUNCH           6
#define CB_FREE             7
#define CB_MALLOC           9
#define CB_THREAD_SYNC      13

#define CB_H2D              1
#define CB_D2H              2


static inline void st_le32(uint8_t *p, uint32_t v)
{
    volatile uint8_t *q = p;
    q[0] = (uint8_t)v;
    q[1] = (uint8_t)(v >> 8);
    q[2] = (uint8_t)(v >> 16);
    q[3] = (uint8_t)(v >> 24);
}

static inline void st_le64(uint8_t *p, uint64_t v)
{
    st_le32(p, (uint32_t)v);
    st_le32(p + 4, (uint32_t)(v >> 32));
}


static inline void mmio_write32(uint32_t addr, uint32_t v)
{
    *(volatile uint32_t*)addr = v;
}

static inline uint32_t mmio_read32(uint32_t addr)
{
    return *(volatile uint32_t*)addr;
}


static inline void cb_memzero(uint8_t *p, uint32_t n)
{
    while (n--)
        *p++ = 0;
}


static inline uint32_t cb_ring(uint8_t *scratch)
{
    mmio_write32(BALAR_DOORBELL, (uint32_t)(uintptr_t)scratch);
    return mmio_read32(BALAR_DOORBELL);
}


#define CB_VEC_N      256u
#define CB_VEC_BYTES  (CB_VEC_N * 4u)
#define CB_SCRATCH_SZ (BP_SIZE + CB_VEC_BYTES + 64u)

static uint8_t  cb_scratch[CB_SCRATCH_SZ] __attribute__((aligned(64)));
static uint32_t cb_a[CB_VEC_N] __attribute__((aligned(64)));
static uint32_t cb_b[CB_VEC_N] __attribute__((aligned(64)));
static uint32_t cb_c[CB_VEC_N] __attribute__((aligned(64)));
static uint8_t  cb_dummy[16];


/* P4 async offload — Quetz-emulated submit/completion aperture (0x100 into
 * balar's MMIO region, which the guest otherwise only touches at offset 0). */
#define BALAR_ASYNC_BASE      (BALAR_DOORBELL + 0x100UL)
#define BALAR_ASYNC_SUBMIT    (BALAR_ASYNC_BASE + 0x00UL)
#define BALAR_ASYNC_STATUS    (BALAR_ASYNC_BASE + 0x08UL)
#define BALAR_ASYNC_COMPLETED (BALAR_ASYNC_BASE + 0x10UL)
#define BALAR_ASYNC_TICKET    (BALAR_ASYNC_BASE + 0x20UL)

/* Dedicated buffer for the posted (in-flight) packet — buffer-lifetime rule:
 * not touched until the op completes. */
static uint8_t cb_async_scratch[BP_SIZE] __attribute__((aligned(64)));

/* Post the scratch-packet address; returns immediately with our ticket. */
static uint32_t cb_submit_async(uint8_t *scratch)
{
    mmio_write32(BALAR_ASYNC_SUBMIT, (uint32_t)(uintptr_t)scratch);
    return mmio_read32(BALAR_ASYNC_TICKET);
}

static void cb_wait_async(uint32_t ticket)
{
    while (mmio_read32(BALAR_ASYNC_COMPLETED) < ticket)
        ;
}

/* Post cudaThreadSynchronize asynchronously; balar holds its deferred response
 * until the launched kernel completes. Returns the ticket. */
static uint32_t cb_sync_async(void)
{
    cb_memzero(cb_async_scratch, BP_SIZE);
    st_le32(cb_async_scratch + BP_CALL_ID, CB_THREAD_SYNC);
    cb_async_scratch[BP_ISSSTMEM] = 0;
    return cb_submit_async(cb_async_scratch);
}

/* CPU work that overlaps the posted kernel; touches no balar scratch buffer. */
static uint32_t cb_cpu_work(uint32_t iters)
{
    uint32_t acc = 1;
    for (uint32_t i = 1; i <= iters; i++)
        acc = acc * 1664525u + 1013904223u + (i ^ (acc >> 7));
    return acc;
}


static void cb_begin(uint32_t call_id, int is_sstmem)
{
    cb_memzero(cb_scratch, BP_SIZE);
    st_le32(cb_scratch + BP_CALL_ID, call_id);
    cb_scratch[BP_ISSSTMEM] = (uint8_t)(is_sstmem ? 1 : 0);
}

static void cb_str(uint32_t off, const char *s)
{
    
    volatile uint8_t *d = cb_scratch + off;
    while (*s)
        *d++ = (uint8_t)*s++;
    *d = 0;
}

static uint32_t cb_reg_fatbin(void)
{
    cb_begin(CB_REG_FAT_BINARY, 0);
    cb_str(BP_FATBIN_NAME, "vectorAdd");
    return cb_ring(cb_scratch);
}

static uint32_t cb_malloc(void)
{
    cb_begin(CB_MALLOC, 0);
    st_le64(cb_scratch + BP_MALLOC_DEVPTR, (uint32_t)(uintptr_t)cb_dummy);
    st_le64(cb_scratch + BP_MALLOC_SIZE, CB_VEC_BYTES);
    return cb_ring(cb_scratch);          
}

static void cb_memcpy_h2d(uint32_t dst_dev, const uint32_t *src)
{
    uint8_t *pl = cb_scratch + BP_SIZE;  
    cb_begin(CB_MEMCPY, 1);
    st_le64(cb_scratch + BP_MEMCPY_DST, dst_dev);
    st_le64(cb_scratch + BP_MEMCPY_SRC, (uint32_t)(uintptr_t)pl);
    st_le64(cb_scratch + BP_MEMCPY_COUNT, CB_VEC_BYTES);
    st_le32(cb_scratch + BP_MEMCPY_KIND, CB_H2D);
    for (uint32_t i = 0; i < CB_VEC_N; i++)
        st_le32(pl + i * 4u, src[i]);     
    (void)cb_ring(cb_scratch);
}

static void cb_memcpy_d2h(uint32_t *dst, uint32_t src_dev)
{
    cb_begin(CB_MEMCPY, 1);
    st_le64(cb_scratch + BP_MEMCPY_DST, (uint32_t)(uintptr_t)dst);
    st_le64(cb_scratch + BP_MEMCPY_SRC, src_dev);
    st_le64(cb_scratch + BP_MEMCPY_COUNT, CB_VEC_BYTES);
    st_le32(cb_scratch + BP_MEMCPY_KIND, CB_D2H);
    (void)cb_ring(cb_scratch);
    
    for (uint32_t i = 0; i < CB_VEC_N; i++)
        dst[i] = mmio_read32(BALAR_DOORBELL);
}

static void cb_reg_function(uint32_t fatbin)
{
    cb_begin(CB_REG_FUNCTION, 0);
    st_le64(cb_scratch + BP_FUNC_HANDLE, fatbin);
    st_le64(cb_scratch + BP_FUNC_HOSTFUN, 0);
    cb_str(BP_FUNC_DEVICEFUN, "_Z6vecAddPiS_S_i");
    (void)cb_ring(cb_scratch);
}

static void cb_config_call(void)
{
    cb_begin(CB_CONFIG_CALL, 0);
    st_le64(cb_scratch + BP_CFG_SHAREDMEM, 0);
    st_le32(cb_scratch + BP_CFG_GDX, 1);
    st_le32(cb_scratch + BP_CFG_GDY, 1);
    st_le32(cb_scratch + BP_CFG_GDZ, 1);
    st_le32(cb_scratch + BP_CFG_BDX, CB_VEC_N);
    st_le32(cb_scratch + BP_CFG_BDY, 1);
    st_le32(cb_scratch + BP_CFG_BDZ, 1);
    (void)cb_ring(cb_scratch);
}

static void cb_set_arg_ptr(uint32_t dev_ptr, uint32_t offset)
{
    cb_begin(CB_SET_ARG, 0);
    st_le64(cb_scratch + BP_ARG_ARG, dev_ptr);   
    st_le64(cb_scratch + BP_ARG_SIZE, 8);
    st_le64(cb_scratch + BP_ARG_OFFSET, offset);
    (void)cb_ring(cb_scratch);
}

static void cb_set_arg_u32(uint32_t value, uint32_t offset)
{
    cb_begin(CB_SET_ARG, 0);
    st_le64(cb_scratch + BP_ARG_SIZE, 4);
    st_le64(cb_scratch + BP_ARG_OFFSET, offset);
    st_le32(cb_scratch + BP_ARG_VALUE, value);   
    (void)cb_ring(cb_scratch);
}

static void cb_launch(void)
{
    cb_begin(CB_LAUNCH, 0);
    st_le64(cb_scratch + BP_LAUNCH_FUNC, 0);
    (void)cb_ring(cb_scratch);
}

static void cb_sync(void)
{
    cb_begin(CB_THREAD_SYNC, 0);
    (void)cb_ring(cb_scratch);
}

static void cb_free(uint32_t dev_ptr)
{
    cb_begin(CB_FREE, 0);
    st_le64(cb_scratch + BP_FREE_DEVPTR, dev_ptr);
    (void)cb_ring(cb_scratch);
}


static uint32_t cb_vadd(void)
{
    uint32_t dev_a, dev_b, dev_c, fatbin, correct = 0;

    for (uint32_t i = 0; i < CB_VEC_N; i++) {
        cb_a[i] = i;
        cb_b[i] = CB_VEC_N - i;
    }

    fatbin = cb_reg_fatbin();
    dev_a  = cb_malloc();
    dev_b  = cb_malloc();
    dev_c  = cb_malloc();

    cb_memcpy_h2d(dev_a, cb_a);
    cb_memcpy_h2d(dev_b, cb_b);

    cb_reg_function(fatbin);
    cb_config_call();
    cb_set_arg_ptr(dev_a, 0);
    cb_set_arg_ptr(dev_b, 8);
    cb_set_arg_ptr(dev_c, 16);
    cb_set_arg_u32(CB_VEC_N, 24);
    cb_launch();
    cb_sync();
    cb_memcpy_d2h(cb_c, dev_c);

    for (uint32_t i = 0; i < CB_VEC_N; i++)
        if (cb_c[i] == cb_a[i] + cb_b[i])
            correct++;

    cb_free(dev_a);
    cb_free(dev_b);
    cb_free(dev_c);
    cb_sync();

    return correct;
}


/* Same vectorAdd, but the post-launch cudaThreadSynchronize is posted via the
 * Quetz async aperture: the vCPU runs cb_cpu_work() while balar runs the kernel,
 * then joins on the ticket. Result must be identical to cb_vadd(). */
static uint32_t cb_vadd_async(void)
{
    uint32_t dev_a, dev_b, dev_c, fatbin, ticket, correct = 0;
    volatile uint32_t acc;

    for (uint32_t i = 0; i < CB_VEC_N; i++) {
        cb_a[i] = i;
        cb_b[i] = CB_VEC_N - i;
    }

    fatbin = cb_reg_fatbin();
    dev_a  = cb_malloc();
    dev_b  = cb_malloc();
    dev_c  = cb_malloc();

    cb_memcpy_h2d(dev_a, cb_a);
    cb_memcpy_h2d(dev_b, cb_b);

    cb_reg_function(fatbin);
    cb_config_call();
    cb_set_arg_ptr(dev_a, 0);
    cb_set_arg_ptr(dev_b, 8);
    cb_set_arg_ptr(dev_c, 16);
    cb_set_arg_u32(CB_VEC_N, 24);
    cb_launch();

    ticket = cb_sync_async();        /* posted — returns immediately */
    acc    = cb_cpu_work(20000u);    /* overlaps the kernel in balar  */
    cb_wait_async(ticket);           /* join                          */

    cb_memcpy_d2h(cb_c, dev_c);

    for (uint32_t i = 0; i < CB_VEC_N; i++)
        if (cb_c[i] == cb_a[i] + cb_b[i])
            correct++;

    cb_free(dev_a);
    cb_free(dev_b);
    cb_free(dev_c);
    cb_sync();

    if (acc == 0)                    /* keep cb_cpu_work from being elided */
        correct = 0;
    return correct;
}

#endif
