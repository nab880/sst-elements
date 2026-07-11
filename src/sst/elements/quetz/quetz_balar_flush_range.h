// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.

#ifndef _H_SST_QUETZ_BALAR_FLUSH_RANGE
#define _H_SST_QUETZ_BALAR_FLUSH_RANGE

#include <cstdint>
#include <limits>

namespace SST {
namespace Quetz {

struct BalarFlushRange {
    uint64_t first_line = 0;
    uint64_t end = 0;
    uint64_t line_count = 0;
};

inline bool computeBalarFlushRange(uint64_t scratch, uint64_t flush_bytes,
                                   uint64_t line_size, BalarFlushRange& range)
{
    const uint64_t max = std::numeric_limits<uint64_t>::max();
    if (line_size == 0 || flush_bytes > max - scratch)
        return false;

    range.first_line = (scratch / line_size) * line_size;
    range.end = scratch + flush_bytes;

    if (flush_bytes == 0) {
        range.line_count = 0;
        return true;
    }

    const uint64_t span = range.end - range.first_line;
    range.line_count = span / line_size + (span % line_size != 0);

    if (range.line_count == 0)
        return true;

    // A FlushAddr covers line_size bytes. Reject a final line whose exclusive
    // end cannot be represented, before issuing any partial set of requests.
    const uint64_t steps = range.line_count - 1;
    if (steps > (max - range.first_line) / line_size)
        return false;
    const uint64_t last_line = range.first_line + steps * line_size;
    return line_size <= max - last_line;
}

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_BALAR_FLUSH_RANGE
