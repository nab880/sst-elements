/*
 * coldfire_devices.h — canonical guest-side register map for the Quetz MMIO
 * devices, plus the 32-bit MMIO accessors, shared by every ColdFire demo and
 * probe firmware. The offsets mirror the device ABIs in quetz_gpu_device.h
 * (REG_*) and quetz_stream_device.h (REG_*) — change them THERE first and
 * here second, never in a per-firmware copy.
 *
 * The base addresses match the ColdFire decks (basic_quetz_gpu_coldfire.py
 * and friends). A firmware whose deck moves an aperture can define GPU_BASE
 * or SENSOR_BASE before including this header.
 */

#ifndef COLDFIRE_DEVICES_H
#define COLDFIRE_DEVICES_H

#include <stdint.h>

static inline void mmio_write32(uint32_t addr, uint32_t v)
{
    *(volatile uint32_t *)addr = v;
}

static inline uint32_t mmio_read32(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

/* QuetzGpuDevice register block (quetz_gpu_device.h REG_*). */
#ifndef GPU_BASE
#define GPU_BASE          0x70000000UL
#endif
#define GPU_DOORBELL      (GPU_BASE + 0x00UL)  /* W: submit */
#define GPU_STATUS        (GPU_BASE + 0x08UL)  /* R: busy(1)/idle(0) */
#define GPU_KERNEL_ID     (GPU_BASE + 0x10UL)  /* R: completed-ticket counter */
#define GPU_LATENCY_OVR   (GPU_BASE + 0x18UL)  /* W: next-kernel cycles */
#define GPU_TICKET        (GPU_BASE + 0x20UL)  /* R: last submit ticket */
#define GPU_RESULT        (GPU_BASE + 0x28UL)  /* R: last completed result */
#define GPU_ARG0          (GPU_BASE + 0x30UL)  /* W: input buffer addr */
#define GPU_ARG1          (GPU_BASE + 0x38UL)  /* W: output buffer addr */
#define GPU_ARG2          (GPU_BASE + 0x40UL)  /* W: kernel-defined (FFT: N) */
#define GPU_ARG3          (GPU_BASE + 0x48UL)  /* W: kernel-defined */
#define GPU_IRQ_ACK       (GPU_BASE + 0x50UL)  /* R: raised; W: consume N */

/* QuetzStreamDevice register block (quetz_stream_device.h REG_*). */
#ifndef SENSOR_BASE
#define SENSOR_BASE       0x70010000UL
#endif
#define SENSOR_STATUS     (SENSOR_BASE + 0x00UL) /* R: bytes ready now */
#define SENSOR_DATA       (SENSOR_BASE + 0x08UL) /* R: pop up to 4 bytes */
#define SENSOR_SEQ        (SENSOR_BASE + 0x10UL) /* R: bytes consumed so far */
#define SENSOR_CTRL       (SENSOR_BASE + 0x18UL) /* W: 1 = rewind to start */
#define SENSOR_EOS        (SENSOR_BASE + 0x20UL) /* R: 1 = fully consumed */
#define SENSOR_IRQ_ACK    (SENSOR_BASE + 0x28UL) /* R: raised; W1: ack */

#endif /* COLDFIRE_DEVICES_H */
