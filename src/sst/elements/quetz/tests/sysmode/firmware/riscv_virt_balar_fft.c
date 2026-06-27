

#include <stddef.h>
#include <stdint.h>

#include "cuda_runtime_types_firmware.h"
#include "../../../../balar/balar_packet_wire.h"
#include "balar_fw_common.h"         
#include "fft_firmware_data_256.h"   

#define FFT_COMPLEX   FFT_N                 
#define VEC_WORDS     (FFT_N * 2u)          
#define VEC_BYTES     (VEC_WORDS * 4u)      
#define TW_WORDS      (FFT_N)               
#define TW_BYTES      (TW_WORDS * 4u)
#define BLOCK_DIM     256u
#define SCRATCH_BYTES 8192u

#define FLOAT_ONE_BITS  0x3F800000u
#define FLOAT_ZERO_BITS 0x00000000u


#define FUNC_BITREV 0u
#define FUNC_STAGE  1u
static const char PTX_BITREV[] = "_Z10fft_bitrevP6float2PKS_ii";
static const char PTX_STAGE[]  = "_Z9fft_stageP6float2PKS_iii";

static uint8_t  scratch[SCRATCH_BYTES] __attribute__((aligned(64)));
static uint32_t host_in[VEC_WORDS]  __attribute__((aligned(64)));
static uint32_t host_out[VEC_WORDS] __attribute__((aligned(64)));

static uint64_t dev_a;     
static uint64_t dev_b;     
static uint64_t dev_tw;    
static uint64_t fatbin_handle;

static uint64_t issue_packet(BalarCudaCallPacket_t *pkt, size_t extra_bytes)
{
    return balar_issue_packet(scratch, SCRATCH_BYTES, pkt, extra_bytes);
}

static void init_input(void)
{
    
    fw_memset(host_in, 0, sizeof(host_in));
    host_in[0] = FLOAT_ONE_BITS;
}

static void packet_reg_fatbin(void)
{
    BalarCudaCallPacket_t pkt;
    fw_memset(&pkt, 0, sizeof(pkt));
    pkt.cuda_call_id = CUDA_REG_FAT_BINARY;
    pkt.isSSTmem = false;
    fw_strcpy(pkt.register_fatbin.file_name, "fft", BALAR_CUDA_MAX_FILE_NAME);
    fatbin_handle = issue_packet(&pkt, 0);
}

static uint64_t packet_malloc(uint64_t size)
{
    uint64_t dev = 0;
    BalarCudaCallPacket_t pkt;
    fw_memset(&pkt, 0, sizeof(pkt));
    pkt.cuda_call_id = CUDA_MALLOC;
    pkt.isSSTmem = false;
    pkt.cuda_malloc.devPtr = (void**)&dev;
    pkt.cuda_malloc.size = size;
    return issue_packet(&pkt, 0);
}

static void packet_memcpy_h2d(uint64_t dst, const void *src, uint64_t nbytes)
{
    BalarCudaCallPacket_t pkt;
    fw_memset(&pkt, 0, sizeof(pkt));
    pkt.cuda_call_id = CUDA_MEMCPY;
    pkt.isSSTmem = true;
    pkt.cuda_memcpy.kind = cudaMemcpyHostToDevice;
    pkt.cuda_memcpy.dst = dst;
    pkt.cuda_memcpy.src = (uint64_t)(uintptr_t)(scratch + sizeof(pkt));
    pkt.cuda_memcpy.count = nbytes;
    pkt.cuda_memcpy.payload = 0;
    fw_memcpy(scratch + sizeof(pkt), src, nbytes);
    (void)issue_packet(&pkt, nbytes);
}

static void packet_memcpy_d2h(void *dst, uint64_t src, uint64_t nbytes)
{
    BalarCudaCallPacket_t pkt;
    fw_memset(&pkt, 0, sizeof(pkt));
    pkt.cuda_call_id = CUDA_MEMCPY;
    pkt.isSSTmem = true;
    pkt.cuda_memcpy.kind = cudaMemcpyDeviceToHost;
    pkt.cuda_memcpy.dst = (uint64_t)(uintptr_t)dst;
    pkt.cuda_memcpy.src = src;
    pkt.cuda_memcpy.count = nbytes;
    pkt.cuda_memcpy.payload = 0;
    (void)issue_packet(&pkt, 0);
    
    for (uint64_t off = 0; off < nbytes; off += sizeof(uint64_t)) {
        uint64_t chunk = mmio_read64(BALAR_DOORBELL);
        uint64_t n = nbytes - off;
        if (n > sizeof(chunk))
            n = sizeof(chunk);
        fw_memcpy((uint8_t*)dst + off, &chunk, n);
    }
}

static void packet_reg_function(const char *deviceFun, uint64_t hostFun)
{
    BalarCudaCallPacket_t pkt;
    fw_memset(&pkt, 0, sizeof(pkt));
    pkt.cuda_call_id = CUDA_REG_FUNCTION;
    pkt.isSSTmem = false;
    pkt.register_function.fatCubinHandle = fatbin_handle;
    pkt.register_function.hostFun = hostFun;
    fw_strcpy(pkt.register_function.deviceFun, deviceFun, BALAR_CUDA_MAX_KERNEL_NAME);
    (void)issue_packet(&pkt, 0);
}

static void packet_config_call(uint32_t gdx, uint32_t bdx)
{
    BalarCudaCallPacket_t pkt;
    fw_memset(&pkt, 0, sizeof(pkt));
    pkt.cuda_call_id = CUDA_CONFIG_CALL;
    pkt.configure_call.gdx = gdx;
    pkt.configure_call.gdy = 1;
    pkt.configure_call.gdz = 1;
    pkt.configure_call.bdx = bdx;
    pkt.configure_call.bdy = 1;
    pkt.configure_call.bdz = 1;
    pkt.configure_call.sharedMem = 0;
    pkt.configure_call.stream = 0;
    (void)issue_packet(&pkt, 0);
}

static void packet_set_arg_ptr(uint64_t value, uint64_t offset)
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

static void packet_launch(uint64_t func)
{
    BalarCudaCallPacket_t pkt;
    fw_memset(&pkt, 0, sizeof(pkt));
    pkt.cuda_call_id = CUDA_LAUNCH;
    pkt.cuda_launch.func = func;
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


static void launch_bitrev(void)
{
    packet_config_call(1, BLOCK_DIM);
    packet_set_arg_ptr(dev_b, 0);
    packet_set_arg_ptr(dev_a, 8);
    packet_set_arg_u32(FFT_N, 16);
    packet_set_arg_u32(FFT_LOGN, 20);
    packet_launch(FUNC_BITREV);
    packet_thread_sync();
}


static void launch_stage(uint32_t s)
{
    packet_config_call(1, BLOCK_DIM);
    packet_set_arg_ptr(dev_b, 0);
    packet_set_arg_ptr(dev_tw, 8);
    packet_set_arg_u32(FFT_N, 16);
    packet_set_arg_u32(s, 20);
    packet_set_arg_u32(FFT_LOGN, 24);
    packet_launch(FUNC_STAGE);
    packet_thread_sync();
}

static uint32_t verify_impulse(void)
{
    uint32_t correct = 0;
    for (uint32_t i = 0; i < VEC_WORDS; i++) {
        uint32_t want = (i & 1u) ? FLOAT_ZERO_BITS : FLOAT_ONE_BITS;
        if (host_out[i] == want)
            correct++;
    }
    return correct;
}

extern char _stack_top[];

__attribute__((naked, used, section(".text.boot"))) void _start(void)
{
    __asm__ volatile(
        "lla sp, _stack_top\n\t"
        "call kernel_main\n\t"
        "1:\twfi\n\t"
        "j 1b\n\t");
}

void kernel_main(void)
{
    init_input();

    packet_reg_fatbin();
    dev_a = packet_malloc(VEC_BYTES);
    dev_b = packet_malloc(VEC_BYTES);
    dev_tw = packet_malloc(TW_BYTES);

    packet_memcpy_h2d(dev_a, host_in, VEC_BYTES);
    packet_memcpy_h2d(dev_tw, fft_tw_bits, TW_BYTES);

    packet_reg_function(PTX_BITREV, FUNC_BITREV);
    packet_reg_function(PTX_STAGE, FUNC_STAGE);

    launch_bitrev();
    for (uint32_t s = 1; s <= FFT_LOGN; s++)
        launch_stage(s);

    packet_memcpy_d2h(host_out, dev_b, VEC_BYTES);

    uint32_t correct = verify_impulse();
    uart_puts("Balar FFT Kernel_done correct_words=");
    uart_put_u64_dec(correct);
    uart_putc('/');
    uart_put_u64_dec(VEC_WORDS);
    uart_putc('\n');

    packet_free(dev_a);
    packet_free(dev_b);
    packet_free(dev_tw);
    packet_thread_sync();

    TESTDEV = (correct == VEC_WORDS) ? TESTDEV_PASS : TESTDEV_FAIL;
    while (1)
        __asm__ volatile ("wfi");
}
