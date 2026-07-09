/*
 * coldfire_scale_ref.h — guest-side mirror of the ScaleOffsetKernel's
 * saturating int16 transform (quetz_scale_offset.h:quetz_scale_offset_sat16)
 * used by the demo/probe firmwares to compute expected values. If the
 * kernel's saturation semantics change, update quetz_scale_offset.h first
 * and this mirror second — it is the single firmware-side copy.
 */

#ifndef COLDFIRE_SCALE_REF_H
#define COLDFIRE_SCALE_REF_H

#include <stdint.h>

static inline int16_t cf_scale_offset_ref(int16_t s, int16_t scale,
                                          int16_t offset)
{
    int32_t v = (int32_t)s * scale + offset;
    if (v > 32767)  return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

#endif /* COLDFIRE_SCALE_REF_H */
