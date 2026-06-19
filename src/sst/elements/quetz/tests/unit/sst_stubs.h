// Shared helpers for Quetz unit tests (mirrors production logic where SST
// SubComponents are too heavy to instantiate in-process).

#ifndef QUETZ_UNIT_SST_STUBS_H
#define QUETZ_UNIT_SST_STUBS_H

#include <cstdint>

namespace quetz_unit {

inline uint32_t slotsNeeded(uint64_t vaddr, uint32_t size, uint64_t cache_line_size) {
    if (size == 0)
        return 1;
    uint64_t first_line = vaddr / cache_line_size;
    uint64_t last_line  = (vaddr + size - 1) / cache_line_size;
    return static_cast<uint32_t>(last_line - first_line + 1);
}

struct RegionRange {
    const char* name;
    uint64_t    start;
    uint64_t    end;
};

inline const RegionRange* findFirstMatch(const RegionRange* regions, size_t count,
                                         uint64_t addr) {
    for (size_t i = 0; i < count; i++) {
        if (addr >= regions[i].start && addr <= regions[i].end)
            return &regions[i];
    }
    return nullptr;
}

} // namespace quetz_unit

#endif
