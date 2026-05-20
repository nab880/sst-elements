// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#ifndef _H_SST_QUETZ_MEMMAP
#define _H_SST_QUETZ_MEMMAP

#include <sst/core/output.h>

#include <stdint.h>
#include <string>
#include <vector>

namespace SST {
namespace Quetz {

enum class MemRegionType {
    MEMORY,
    FILTERED,
    UART,
};

struct MemRegion {
    std::string    name;
    uint64_t       start;
    uint64_t       end;
    MemRegionType  type;
    uint32_t       uart_tx_offset;
};

class MemMap {
public:
    explicit MemMap(std::vector<MemRegion> regions);

    const MemRegion* findRegion(uint64_t vaddr) const;
    bool             isFiltered(uint64_t vaddr) const;
    bool             captureUartByte(uint64_t addr, uint8_t byte);
    void             flushUart(SST::Output* out, uint32_t core_id) const;
    size_t           regionCount() const { return regions_.size(); }

private:
    std::vector<MemRegion> regions_;
    std::string            uart_tx_buf_;
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_MEMMAP
