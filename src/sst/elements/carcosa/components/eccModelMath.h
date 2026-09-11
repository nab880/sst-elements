#ifndef SST_ELEMENTS_CARCOSA_ECC_MODEL_MATH_H
#define SST_ELEMENTS_CARCOSA_ECC_MODEL_MATH_H

#include "sst/elements/carcosa/components/eccScheme.h"
#include <algorithm>
#include <cstdint>
#include <string>

namespace SST { namespace Carcosa { namespace EccModelMath {

inline uint32_t wordCount(uint32_t payload_bytes, EccScheme scheme) {
    const uint32_t wb = eccWordBytes(scheme);
    if (wb == 0 || payload_bytes == 0) return payload_bytes ? 1u : 0u;
    return (payload_bytes + wb - 1) / wb;
}

inline uint32_t wordBits(uint32_t payload_bytes, EccScheme scheme,
                         uint32_t word_index) {
    const uint32_t wb = eccWordBytes(scheme);
    if (wb == 0) return word_index == 0 ? payload_bytes * 8 : 0;
    const uint64_t used = static_cast<uint64_t>(word_index) * wb;
    if (used >= payload_bytes) return 0;
    return static_cast<uint32_t>(std::min<uint64_t>(wb, payload_bytes - used) * 8);
}

}}} // namespace SST::Carcosa::EccModelMath

#endif
