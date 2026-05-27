/*
 * riscv_virt_balar_kernel.c — bare-metal Balar/GPGPU-Sim doorbell driver.
 *
 * Runs a small vectorAdd stream from a QEMU sysmode guest by constructing
 * BalarCudaCallPacket_t packets in guest DRAM and ringing balar.balarMMIO.
 */

#include <stddef.h>
#include <stdint.h>

#include "cuda_runtime_types_firmware.h"
#include "../../../../balar/balar_packet_wire.h"

#define BALAR_DOORBELL 0x70000000UL
#define UART0_BASE     0x10000000UL
#define UART_THR       (*(volatile unsigned char*)(UART0_BASE + 0x00))
#define UART_LSR       (*(volatile unsigned char*)(UART0_BASE + 0x05))
#define LSR_THRE       (1u << 5)

#define TESTDEV      (*(volatile unsigned int*)0x100000UL)
#define TESTDEV_PASS 0x5555u
#define TESTDEV_FAIL 0x3333u

#define VECTOR_N      256u
#define VECTOR_BYTES  (VECTOR_N * sizeof(uint32_t))
#define SCRATCH_BYTES 4096u

static uint8_t scratch[SCRATCH_BYTES] __attribute__((aligned(64)));
static uint32_t host_a[VECTOR_N] __attribute__((aligned(64)));
static uint32_t host_b[VECTOR_N] __attribute__((aligned(64)));
static uint32_t host_c[VECTOR_N] __attribute__((aligned(64)));

static uint64_t dev_a;
static uint64_t dev_b;
static uint64_t dev_c;
static uint64_t fatbin_handle;

static void uart_putc(char c)
{
    while (!(UART_LSR & LSR_THRE))
        ;
    UART_THR = (unsigned char)c;
}

static void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

static void uart_put_u64_dec(uint64_t v)
{
    char buf[32];
    unsigned i = 0;
    if (v == 0) {
        uart_putc('0');
        return;
    }
    while (v && i < sizeof(buf)) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i)
        uart_putc(buf[--i]);
}

static void *fw_memset(void *dst, int val, size_t n)
{
    uint8_t *p = (uint8_t*)dst;
    while (n--)
        *p++ = (uint8_t)val;
    return dst;
}

static void *fw_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t*)dst;
    const uint8_t *s = (const uint8_t*)src;
    while (n--)
        *d++ = *s++;
    return dst;
}

static void fw_strcpy(char *dst, const char *src, size_t cap)
{
    if (cap == 0)
        return;
    while (cap > 1 && *src) {
        *dst++ = *src++;
        cap--;
    }
    *dst = '\0';
}

static void mmio_write64(uint64_t addr, uint64_t value)
{
    *(volatile uint64_t*)(uintptr_t)addr = value;
}

static uint64_t mmio_read64(uint64_t addr)
{
    return *(volatile uint64_t*)(uintptr_t)addr;
}

static BalarCudaCallReturnPacket_t *issue_packet(BalarCudaCallPacket_t *pkt, size_t extra_bytes)
{
    size_t total = sizeof(*pkt) + extra_bytes;
    if (total > SCRATCH_BYTES) {
        uart_puts("balar scratch overflow\n");
        TESTDEV = TESTDEV_FAIL;
        while (1)
            __asm__ volatile ("wfi");
    }

    fw_memcpy(scratch, pkt, sizeof(*pkt));
    mmio_write64(BALAR_DOORBELL, (uint64_t)(uintptr_t)scratch);
    uint64_t ret_addr = mmio_read64(BALAR_DOORBELL);
    return (BalarCudaCallReturnPacket_t*)(uintptr_t)ret_addr;
}

static void init_vectors(void)
{
    for (uint32_t i = 0; i < VECTOR_N; i++) {
        host_a[i] = i;
        host_b[i] = VECTOR_N - i;
        host_c[i] = 0;
    }
}

static void packet_reg_fatbin(void)
{
    BalarCudaCallPacket_t pkt;
    fw_memset(&pkt, 0, sizeof(pkt));
    pkt.cuda_call_id = CUDA_REG_FAT_BINARY;
    pkt.isSSTmem = false;
    fw_strcpy(pkt.register_fatbin.file_name, "vectorAdd", BALAR_CUDA_MAX_FILE_NAME);

    BalarCudaCallReturnPacket_t *ret = issue_packet(&pkt, 0);
    fatbin_handle = ret->fat_cubin_handle;
}

static uint64_t packet_malloc(void)
{
    uint64_t dev = 0;
    BalarCudaCallPacket_t pkt;
    fw_memset(&pkt, 0, sizeof(pkt));
    pkt.cuda_call_id = CUDA_MALLOC;
    pkt.isSSTmem = false;
    pkt.cuda_malloc.devPtr = (void**)&dev;
    pkt.cuda_malloc.size = VECTOR_BYTES;

    BalarCudaCallReturnPacket_t *ret = issue_packet(&pkt, 0);
    return ret->cudamalloc.malloc_addr;
}

static void packet_memcpy_h2d(uint64_t dst, const uint32_t *src)
{
    BalarCudaCallPacket_t pkt;
    fw_memset(&pkt, 0, sizeof(pkt));
    pkt.cuda_call_id = CUDA_MEMCPY;
    pkt.isSSTmem = true;
    pkt.cuda_memcpy.kind = cudaMemcpyHostToDevice;
    pkt.cuda_memcpy.dst = dst;
    pkt.cuda_memcpy.src = (uint64_t)(uintptr_t)(scratch + sizeof(pkt));
    pkt.cuda_memcpy.count = VECTOR_BYTES;
    pkt.cuda_memcpy.payload = 0;

    fw_memcpy(scratch + sizeof(pkt), src, VECTOR_BYTES);
    (void)issue_packet(&pkt, VECTOR_BYTES);
}

static void packet_memcpy_d2h(uint32_t *dst, uint64_t src)
{
    BalarCudaCallPacket_t pkt;
    fw_memset(&pkt, 0, sizeof(pkt));
    pkt.cuda_call_id = CUDA_MEMCPY;
    pkt.isSSTmem = true;
    pkt.cuda_memcpy.kind = cudaMemcpyDeviceToHost;
    pkt.cuda_memcpy.dst = (uint64_t)(uintptr_t)dst;
    pkt.cuda_memcpy.src = src;
    pkt.cuda_memcpy.count = VECTOR_BYTES;
    pkt.cuda_memcpy.payload = 0;

    (void)issue_packet(&pkt, 0);
}

static void packet_reg_function(void)
{
    BalarCudaCallPacket_t pkt;
    fw_memset(&pkt, 0, sizeof(pkt));
    pkt.cuda_call_id = CUDA_REG_FUNCTION;
    pkt.isSSTmem = false;
    pkt.register_function.fatCubinHandle = fatbin_handle;
    pkt.register_function.hostFun = 0;
    fw_strcpy(pkt.register_function.deviceFun, "_Z6vecAddPiS_S_i",
              BALAR_CUDA_MAX_KERNEL_NAME);
    (void)issue_packet(&pkt, 0);
}

static void packet_config_call(void)
{
    BalarCudaCallPacket_t pkt;
    fw_memset(&pkt, 0, sizeof(pkt));
    pkt.cuda_call_id = CUDA_CONFIG_CALL;
    pkt.configure_call.gdx = 1;
    pkt.configure_call.gdy = 1;
    pkt.configure_call.gdz = 1;
    pkt.configure_call.bdx = 256;
    pkt.configure_call.bdy = 1;
    pkt.configure_call.bdz = 1;
    pkt.configure_call.sharedMem = 0;
    pkt.configure_call.stream = 0;
    (void)issue_packet(&pkt, 0);
}

static void packet_set_arg_u64(uint64_t value, uint64_t offset)
{
    BalarCudaCallPacket_t pkt;
    fw_memset(&pkt, 0, sizeof(pkt));
    pkt.cuda_call_id = CUDA_SET_ARG;
    pkt.setup_argument.size = sizeof(uint64_t);
    pkt.setup_argument.offset = offset;
    pkt.setup_argument.arg = value;
    (void)issue_packet(&pkt, 0);
}

static void packet_set_arg_u32(uint32_t value, uint64_t offset)
{
    BalarCudaCallPacket_t pkt;
    fw_memset(&pkt, 0, sizeof(pkt));
    pkt.cuda_call_id = CUDA_SET_ARG;
    pkt.setup_argument.size = sizeof(uint32_t);
    pkt.setup_argument.offset = offset;
    pkt.setup_argument.arg = 0;
    fw_memcpy(pkt.setup_argument.value, &value, sizeof(value));
    (void)issue_packet(&pkt, 0);
}

static void packet_launch(void)
{
    BalarCudaCallPacket_t pkt;
    fw_memset(&pkt, 0, sizeof(pkt));
    pkt.cuda_call_id = CUDA_LAUNCH;
    pkt.cuda_launch.func = 0;
    (void)issue_packet(&pkt, 0);
}

static void packet_thread_sync(void)
{
    BalarCudaCallPacket_t pkt;
    fw_memset(&pkt, 0, sizeof(pkt));
    pkt.cuda_call_id = CUDA_THREAD_SYNC;
    (void)issue_packet(&pkt, 0);
}

static void packet_free(uint64_t dev)
{
    BalarCudaCallPacket_t pkt;
    fw_memset(&pkt, 0, sizeof(pkt));
    pkt.cuda_call_id = CUDA_FREE;
    pkt.cuda_free.devPtr = (void*)(uintptr_t)dev;
    (void)issue_packet(&pkt, 0);
}

static uint32_t verify_result(void)
{
    uint32_t correct = 0;
    for (uint32_t i = 0; i < VECTOR_N; i++) {
        if (host_c[i] == host_a[i] + host_b[i])
            correct++;
    }
    return correct;
}

void _start(void)
{
    init_vectors();

    packet_reg_fatbin();
    dev_a = packet_malloc();
    dev_b = packet_malloc();
    dev_c = packet_malloc();

    packet_memcpy_h2d(dev_a, host_a);
    packet_memcpy_h2d(dev_b, host_b);

    packet_reg_function();
    packet_config_call();
    packet_set_arg_u64(dev_a, 0);
    packet_set_arg_u64(dev_b, 8);
    packet_set_arg_u64(dev_c, 16);
    packet_set_arg_u32(VECTOR_N, 24);
    packet_launch();
    packet_thread_sync();
    packet_memcpy_d2h(host_c, dev_c);

    uint32_t correct = verify_result();
    uart_puts("Balar vectorAdd Kernel_done correct_memD2H_ratio=");
    uart_put_u64_dec(correct);
    uart_putc('/');
    uart_put_u64_dec(VECTOR_N);
    uart_putc('\n');

    packet_free(dev_a);
    packet_free(dev_b);
    packet_free(dev_c);
    packet_thread_sync();

    TESTDEV = (correct * 100u >= VECTOR_N * 95u) ? TESTDEV_PASS : TESTDEV_FAIL;
    while (1)
        __asm__ volatile ("wfi");
}
