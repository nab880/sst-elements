/* Vendored copy of sst-hgcc hg_cuda.h; keep token-identical (ABI version asserted in gpu_library.cc). */
#ifndef SST_HG_CUDA_H
#define SST_HG_CUDA_H

#include <stdint.h>

#define SST_HG_CUDA_ABI_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

void* sst_hg_cuda_malloc(uint64_t bytes); /* cookie in reserved range */
void  sst_hg_cuda_free(void* dptr);
int   sst_hg_cuda_is_device_ptr(const void* p);
void  sst_hg_cuda_memcpy(void* dst, const void* src, uint64_t bytes,
                         int kind /* cudaMemcpyKind */,
                         void* stream /* 0 = default */);
void  sst_hg_cuda_launch(const char* kernelName,
                         uint32_t gx, uint32_t gy, uint32_t gz,
                         uint32_t bx, uint32_t by, uint32_t bz,
                         uint64_t shmemBytes, void* stream,
                         /* per-thread costs; may be overridden by calibration table */
                         uint64_t flops, uint64_t intops,
                         uint64_t bytesRead, uint64_t bytesWritten);
void* sst_hg_cuda_stream_create(void);
void  sst_hg_cuda_stream_destroy(void* s);
void  sst_hg_cuda_stream_sync(void* s);
void* sst_hg_cuda_event_create(void);
void  sst_hg_cuda_event_record(void* evt, void* stream);
void  sst_hg_cuda_event_sync(void* evt);
float sst_hg_cuda_event_elapsed_ms(void* start, void* stop);
void  sst_hg_cuda_device_sync(void);
int   sst_hg_cuda_get_device_count(void);
void  sst_hg_cuda_set_device(int dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
