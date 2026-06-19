// Minimal CUDA runtime type shims for bare-metal Quetz firmware.
//
// The firmware only constructs Balar wire packets; it never calls a CUDA
// runtime. These POD definitions are intentionally limited to the fields that
// make balar_packet_wire.h compile in a freestanding RV64 build.

#ifndef CUDA_RUNTIME_TYPES_FIRMWARE_H
#define CUDA_RUNTIME_TYPES_FIRMWARE_H

#include <stddef.h>
#include <stdint.h>

typedef int cudaError_t;
typedef void* cudaStream_t;
typedef void* cudaEvent_t;
typedef int cudaDeviceAttr;

enum cudaMemcpyKind {
    cudaMemcpyHostToHost = 0,
    cudaMemcpyHostToDevice = 1,
    cudaMemcpyDeviceToHost = 2,
    cudaMemcpyDeviceToDevice = 3,
    cudaMemcpyDefault = 4,
};

typedef struct dim3 {
    unsigned int x;
    unsigned int y;
    unsigned int z;
} dim3;

struct textureReference {
    uint8_t reserved[128];
};

struct cudaChannelFormatDesc {
    int x;
    int y;
    int z;
    int w;
    int f;
};

struct cudaDeviceProp {
    uint8_t reserved[1024];
};

#endif // CUDA_RUNTIME_TYPES_FIRMWARE_H
