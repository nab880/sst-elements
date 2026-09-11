// ColdFire cache-op encoding only; no QEMU or SST dependency.
#ifndef QUETZ_COLDFIRE_CACHE_OPS_H
#define QUETZ_COLDFIRE_CACHE_OPS_H

#include <cstddef>
#include <cstdint>

namespace SST { namespace Quetz {
struct ColdFireCacheOp {
    bool valid = false;
    uint32_t kind = 0;       // mailbox size: 0 MOVEC, 1 CPUSHL
    uint16_t control = 0;    // mailbox addr: CACR or ACR number
    uint16_t instruction = 0;
    unsigned source = 0;    // d0-d7, a0-a7 in encoding order
};

inline ColdFireCacheOp decodeColdFireCacheOp(const uint8_t* bytes, size_t size)
{
    ColdFireCacheOp op;
    if (size < 2) return op;
    op.instruction = (uint16_t(bytes[0]) << 8) | bytes[1];
    if (op.instruction == 0x4e7b && size >= 4) {
        const uint16_t ext = (uint16_t(bytes[2]) << 8) | bytes[3];
        op.control = ext & 0x0fff;
        op.source = ext >> 12;
        op.valid = op.control == 0x002 ||
                   (op.control >= 0x004 && op.control <= 0x007);
    } else if ((op.instruction & 0xff38) == 0xf428) {
        // ColdFire uses a set/way operand, not the 68040 physical address.
        op.valid = true;
        op.kind = 1;
        op.source = 8 + (op.instruction & 7);
    }
    return op;
}
} }
#endif
