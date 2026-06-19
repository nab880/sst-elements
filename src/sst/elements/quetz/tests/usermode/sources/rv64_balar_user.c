/*
 * rv64_balar_user.c — P6 Phase 1 user-mode balar vectorAdd offload.
 *
 * Same CUDA-call doorbell protocol as sysmode/firmware/riscv_virt_balar_kernel.c,
 * but a normal Linux RV64 process: no -kernel, no UART/TESTDEV, output via
 * write(). The balar MMIO doorbell at BALAR_DOORBELL is reserved PROT_NONE by
 * qemu-riscv64 (-sst-mmio-range) and each access faults into the linux-user
 * SIGSEGV handler -> sync mailbox -> BalarAcceleratorPort (flush + forward).
 *
 * In user mode the guest VA is the SST memory address, so balar's DMA of the
 * scratch packet (and H2D/D2H buffers) reads the same bytes the trace stream
 * wrote; the doorbell-flush keeps them coherent before each forward.
 */

#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include "cuda_runtime_types_firmware.h"
#include "balar_packet_wire.h"

#define BALAR_DOORBELL  0x70000000UL
#define VECTOR_N        256u
#define VECTOR_BYTES    (VECTOR_N * sizeof(uint32_t))
#define SCRATCH_BYTES   4096u

static uint8_t  scratch[SCRATCH_BYTES] __attribute__((aligned(64)));
static uint32_t host_a[VECTOR_N] __attribute__((aligned(64)));
static uint32_t host_b[VECTOR_N] __attribute__((aligned(64)));
static uint32_t host_c[VECTOR_N] __attribute__((aligned(64)));

static uint64_t dev_a, dev_b, dev_c, fatbin_handle;

static void *u_memset(void *d, int v, size_t n)
{
    uint8_t *p = d;
    while (n--) *p++ = (uint8_t)v;
    return d;
}
static void *u_memcpy(void *d, const void *s, size_t n)
{
    uint8_t *dp = d; const uint8_t *sp = s;
    while (n--) *dp++ = *sp++;
    return d;
}
static void u_strcpy(char *d, const char *s, size_t cap)
{
    if (!cap) return;
    while (cap > 1 && *s) { *d++ = *s++; cap--; }
    *d = 0;
}
static inline void mmio_write64(uint64_t a, uint64_t v)
{
    *(volatile uint64_t *)(uintptr_t)a = v;
}
static inline uint64_t mmio_read64(uint64_t a)
{
    return *(volatile uint64_t *)(uintptr_t)a;
}

static uint64_t issue_packet(BalarCudaCallPacket_t *pkt, size_t extra)
{
    u_memcpy(scratch, pkt, sizeof(*pkt));
    mmio_write64(BALAR_DOORBELL, (uint64_t)(uintptr_t)scratch);
    (void)extra;
    return mmio_read64(BALAR_DOORBELL);
}

static void put_str(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    (void)!write(1, s, n);
}
static void put_dec(uint64_t v)
{
    char b[20]; int t = 0;
    if (!v) { (void)!write(1, "0", 1); return; }
    while (v) { b[t++] = (char)('0' + v % 10); v /= 10; }
    char o[20]; int n = 0;
    while (t) o[n++] = b[--t];
    (void)!write(1, o, n);
}

int main(void)
{
    for (uint32_t i = 0; i < VECTOR_N; i++) {
        host_a[i] = i;
        host_b[i] = VECTOR_N - i;
    }

    BalarCudaCallPacket_t pkt;

    u_memset(&pkt, 0, sizeof(pkt));
    pkt.cuda_call_id = CUDA_REG_FAT_BINARY;
    pkt.isSSTmem = false;
    u_strcpy(pkt.register_fatbin.file_name, "vectorAdd", BALAR_CUDA_MAX_FILE_NAME);
    fatbin_handle = issue_packet(&pkt, 0);

    for (int m = 0; m < 3; m++) {
        u_memset(&pkt, 0, sizeof(pkt));
        pkt.cuda_call_id = CUDA_MALLOC;
        pkt.isSSTmem = false;
        uint64_t dev = 0;
        pkt.cuda_malloc.devPtr = (void **)&dev;
        pkt.cuda_malloc.size = VECTOR_BYTES;
        uint64_t r = issue_packet(&pkt, 0);
        if (m == 0) dev_a = r;
        else if (m == 1) dev_b = r;
        else dev_c = r;
    }

    /* H2D for a and b */
    uint64_t srcs[2] = { (uint64_t)(uintptr_t)host_a, (uint64_t)(uintptr_t)host_b };
    uint64_t dsts[2] = { dev_a, dev_b };
    for (int k = 0; k < 2; k++) {
        u_memset(&pkt, 0, sizeof(pkt));
        pkt.cuda_call_id = CUDA_MEMCPY;
        pkt.isSSTmem = true;
        pkt.cuda_memcpy.kind = cudaMemcpyHostToDevice;
        pkt.cuda_memcpy.dst = dsts[k];
        pkt.cuda_memcpy.src = (uint64_t)(uintptr_t)(scratch + sizeof(pkt));
        pkt.cuda_memcpy.count = VECTOR_BYTES;
        pkt.cuda_memcpy.payload = 0;
        u_memcpy(scratch + sizeof(pkt), (const void *)(uintptr_t)srcs[k], VECTOR_BYTES);
        (void)issue_packet(&pkt, VECTOR_BYTES);
    }

    u_memset(&pkt, 0, sizeof(pkt));
    pkt.cuda_call_id = CUDA_REG_FUNCTION;
    pkt.isSSTmem = false;
    pkt.register_function.fatCubinHandle = fatbin_handle;
    pkt.register_function.hostFun = 0;
    u_strcpy(pkt.register_function.deviceFun, "_Z6vecAddPiS_S_i",
             BALAR_CUDA_MAX_KERNEL_NAME);
    (void)issue_packet(&pkt, 0);

    u_memset(&pkt, 0, sizeof(pkt));
    pkt.cuda_call_id = CUDA_CONFIG_CALL;
    pkt.configure_call.gdx = 1; pkt.configure_call.gdy = 1; pkt.configure_call.gdz = 1;
    pkt.configure_call.bdx = 256; pkt.configure_call.bdy = 1; pkt.configure_call.bdz = 1;
    pkt.configure_call.sharedMem = 0; pkt.configure_call.stream = 0;
    (void)issue_packet(&pkt, 0);

    /* set_arg: three u64 device pointers then one u32 length */
    uint64_t args[3] = { dev_a, dev_b, dev_c };
    for (int a = 0; a < 3; a++) {
        u_memset(&pkt, 0, sizeof(pkt));
        pkt.cuda_call_id = CUDA_SET_ARG;
        pkt.setup_argument.size = sizeof(uint64_t);
        pkt.setup_argument.offset = (uint64_t)(a * 8);
        pkt.setup_argument.arg = args[a];
        (void)issue_packet(&pkt, 0);
    }
    u_memset(&pkt, 0, sizeof(pkt));
    pkt.cuda_call_id = CUDA_SET_ARG;
    pkt.setup_argument.size = sizeof(uint32_t);
    pkt.setup_argument.offset = 24;
    pkt.setup_argument.arg = 0;
    {
        uint32_t n = VECTOR_N;
        u_memcpy(pkt.setup_argument.value, &n, sizeof(n));
    }
    (void)issue_packet(&pkt, 0);

    u_memset(&pkt, 0, sizeof(pkt));
    pkt.cuda_call_id = CUDA_LAUNCH;
    pkt.cuda_launch.func = 0;
    (void)issue_packet(&pkt, 0);

    u_memset(&pkt, 0, sizeof(pkt));
    pkt.cuda_call_id = CUDA_THREAD_SYNC;
    (void)issue_packet(&pkt, 0);

    /* D2H: issue packet then stream the result back through the doorbell. */
    u_memset(&pkt, 0, sizeof(pkt));
    pkt.cuda_call_id = CUDA_MEMCPY;
    pkt.isSSTmem = true;
    pkt.cuda_memcpy.kind = cudaMemcpyDeviceToHost;
    pkt.cuda_memcpy.dst = (uint64_t)(uintptr_t)host_c;
    pkt.cuda_memcpy.src = dev_c;
    pkt.cuda_memcpy.count = VECTOR_BYTES;
    pkt.cuda_memcpy.payload = 0;
    (void)issue_packet(&pkt, 0);
    for (size_t off = 0; off < VECTOR_BYTES; off += sizeof(uint64_t)) {
        uint64_t chunk = mmio_read64(BALAR_DOORBELL);
        size_t n = VECTOR_BYTES - off;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        u_memcpy((uint8_t *)host_c + off, &chunk, n);
    }

    uint32_t correct = 0;
    for (uint32_t i = 0; i < VECTOR_N; i++) {
        if (host_c[i] == host_a[i] + host_b[i]) correct++;
    }

    put_str("Balar vectorAdd user-mode correct=");
    put_dec(correct);
    put_str("/");
    put_dec(VECTOR_N);
    put_str("\n");

    return (correct == VECTOR_N) ? 0 : 1;
}
