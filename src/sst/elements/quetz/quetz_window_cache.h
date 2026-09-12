// Copyright (c) 2026, NTESS. All rights reserved.
#ifndef SST_QUETZ_WINDOW_CACHE_H
#define SST_QUETZ_WINDOW_CACHE_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace SST { namespace Quetz {

// Functional MCF548x/V4e data-cache proxy for registered physical regions,
// independent of the trace cache. Geometry and maintenance follow NXP
// MCF5485RM Rev.5 sections 7.8--7.11 and CFPRM Rev.3, CPUSHL (8-2).
// This geometry is an explicit proxy, not a verified Raptor silicon claim.
// Lowest invalid way wins; otherwise a cache-wide 2-bit allocation counter
// selects the victim. Read/write hits do not advance it (pseudo-round-robin).
// Timing, nonblocking fills, store buffering, and bus snooping are not modeled.
// The caller serializes guest transactions and acknowledges maintenance only
// after every returned backing-memory write has completed.
class WindowDataCache {
public:
    static constexpr uint32_t LineBytes = 16;
    static constexpr uint32_t Sets = 512;
    static constexpr uint32_t Ways = 4;
    static constexpr uint32_t CapacityBytes = Sets * Ways * LineBytes;
    static constexpr uint32_t DEC = 0x80000000u;
    static constexpr uint32_t DDPI = 0x10000000u;
    static constexpr uint32_t DHLCK = 0x08000000u;
    static constexpr uint32_t DCINVA = 0x01000000u;
    enum class Kind { Done, Read, Write };
    struct Action {
        Kind kind;
        uint64_t address;
        uint32_t size;
        std::vector<uint8_t> bytes;
    };

    void configure(uint64_t base, uint64_t size) {
        if (active_ || !regions_.empty())
            throw std::logic_error("window cache geometry cannot be changed after configuration");
        addRegion(base, size);
    }
    // All backing regions share this one cache's capacity, tags and controls.
    // A caller may route returned memory actions to different backing stores.
    void addRegion(uint64_t base, uint64_t size) {
        if (active_) throw std::logic_error("region change during a window cache transaction");
        if (!size || size > 1024 * 1024 || base > UINT32_MAX ||
            size - 1 > UINT32_MAX - base || base % LineBytes || size % LineBytes)
            throw std::invalid_argument("window cache requires an aligned 16-byte to 1-MiB 32-bit window");
        for (const auto& region : regions_)
            if (base < region.base + region.size && region.base < base + size)
                throw std::invalid_argument("overlapping window cache regions");
        regions_.push_back({base, size});
    }
    // Hardware reset clears controls, not cache tags/data (MCF5485RM 7.10.1).
    // Software must issue DCINVA before enabling potentially stale cache data.
    void reset() {
        if (active_) throw std::logic_error("reset during a window cache transaction");
        cacr_ = 0;
        acr_.fill(0);
    }
    bool contains(uint64_t address) const {
        return regionFor(address) != nullptr;
    }
    bool active() const { return active_; }
    uint64_t dirtyDiscards() const { return dirty_discards_; }

    void access(uint64_t address, uint32_t size, bool write,
                const std::vector<uint8_t>& bytes = {}) {
        const auto* region = regionFor(address);
        if (!size || size > 8 || !region ||
            size > region->size - (address - region->base) || (write && bytes.size() != size))
            throw std::invalid_argument("invalid window cache access");
        start();
        address_ = address; count_ = size; offset_ = 0; write_ = write;
        bytes_ = write ? bytes : std::vector<uint8_t>(size);
    }

    // MOVEC CACR/ACR0-3. Supervisor matching only; instruction-cache controls
    // are retained but have no data effect. Data write protection and deferred
    // store buffering fail closed rather than silently claiming their behavior.
    void movec(uint32_t reg, uint32_t value) {
        if (reg != 2 && !(reg >= 4 && reg <= 7))
            throw std::invalid_argument("unsupported ColdFire cache control register");
        if (reg == 2 && (value & 0x60000000u))
            throw std::invalid_argument("unsupported CACR write-protection/store-buffer controls");
        if ((reg == 4 || reg == 5) && (value & 4u))
            throw std::invalid_argument("unsupported data ACR write protection");
        start();
        if (reg == 2) {
            if (value & DCINVA) {
                for (auto& line : lines_) {
                    dirty_discards_ += line.valid && line.dirty;
                    line.valid = line.dirty = false;
                } // invalidate discards, it does NOT write back
            }
            cacr_ = value & ~(DCINVA | 0x00040100u); // invalidate bits self-clear
        } else {
            acr_[reg - 4] = value;
        }
    }

    // CPUSHL addresses the directory: An[12:4] selects a set and An[1:0] a way.
    // Tag bits and An[3:2] do not participate. DEC does not gate maintenance.
    // DDPI suppresses invalidation after a dirty line is written back.
    void push(uint16_t instruction, uint32_t operand) {
        if ((instruction & 0xff38u) != 0xf428u || !(instruction & 0xc0u))
            throw std::invalid_argument("invalid ColdFire CPUSHL instruction");
        start();
        if (!(instruction & 0x40u)) return; // instruction cache only
        pushing_ = true;
        invalidate_push_ = !(cacr_ & DDPI);
        pending_slot_ = setIndex(operand) * Ways + (operand & (Ways - 1));
    }

    Action next() {
        if (!active_ || pending_ != Pending::None)
            throw std::logic_error("window cache transaction ordering error");
        if (pushing_) {
            auto& line = lines_[pending_slot_];
            if (line.valid && line.dirty) {
                pending_ = Pending::Push;
                return {Kind::Write, line.address, LineBytes,
                        {line.bytes.begin(), line.bytes.end()}};
            }
            if (invalidate_push_) line.valid = false;
            pushing_ = false;
        } else {
            while (offset_ < count_) {
                const uint64_t address = address_ + offset_;
                const uint64_t line_address = address & ~(uint64_t(LineBytes) - 1);
                const uint32_t in_line = address - line_address;
                pending_size_ = std::min(count_ - offset_, LineBytes - in_line);
                pending_address_ = line_address;
                const uint32_t cache_mode = mode(address);
                uint32_t slot = find(line_address);
                if (cache_mode >= 2 || (write_ && cache_mode == 0 && slot == NoSlot)) {
                    pending_ = write_ ? Pending::BypassWrite : Pending::BypassRead;
                    return {write_ ? Kind::Write : Kind::Read, address, pending_size_,
                        write_ ? std::vector<uint8_t>(bytes_.begin() + offset_,
                            bytes_.begin() + offset_ + pending_size_) : std::vector<uint8_t>{}};
                }
                if (slot == NoSlot) {
                    pending_slot_ = victim(setIndex(line_address));
                    auto& old = lines_[pending_slot_];
                    if (old.valid && old.dirty) {
                        // Conservative serialized push buffer: acknowledge the
                        // victim before issuing the incoming line fill. Hardware
                        // may overlap these; no latency or overlap claim here.
                        pending_ = Pending::Evict;
                        return {Kind::Write, old.address, LineBytes,
                                {old.bytes.begin(), old.bytes.end()}};
                    }
                    pending_ = Pending::Fill;
                    return {Kind::Read, line_address, LineBytes, {}};
                }
                auto& line = lines_[slot];
                if (write_) {
                    std::copy_n(bytes_.begin() + offset_, pending_size_,
                                line.bytes.begin() + in_line);
                    if (cache_mode == 0) { // write-through
                        pending_ = Pending::WriteThrough;
                        pending_slot_ = slot;
                        return {Kind::Write, address, pending_size_,
                            {bytes_.begin() + offset_, bytes_.begin() + offset_ + pending_size_}};
                    }
                    line.dirty = true;
                } else {
                    std::copy_n(line.bytes.begin() + in_line, pending_size_,
                                bytes_.begin() + offset_);
                }
                offset_ += pending_size_;
            }
        }
        active_ = false;
        return {Kind::Done, 0, 0, write_ ? std::vector<uint8_t>{} : bytes_};
    }

    void complete(const std::vector<uint8_t>& bytes = {}) {
        if (!active_ || pending_ == Pending::None)
            throw std::logic_error("unexpected window cache memory response");
        if (pending_ == Pending::Fill) {
            if (bytes.size() != LineBytes) throw std::runtime_error("short window cache line fill");
            Line line;
            std::copy(bytes.begin(), bytes.end(), line.bytes.begin());
            line.address = pending_address_;
            line.valid = true;
            lines_[pending_slot_] = line;
            replacement_ = (replacement_ + 1) & (Ways - 1);
        } else if (pending_ == Pending::BypassRead) {
            if (bytes.size() != pending_size_) throw std::runtime_error("short window cache read");
            std::copy(bytes.begin(), bytes.end(), bytes_.begin() + offset_);
            offset_ += pending_size_;
        } else if (pending_ == Pending::Push) {
            auto& line = lines_[pending_slot_];
            line.dirty = false;
            if (invalidate_push_) line.valid = false;
            pushing_ = false;
        } else if (pending_ == Pending::Evict) {
            lines_[pending_slot_].valid = lines_[pending_slot_].dirty = false;
        } else {
            // MCF5485RM Table 7-10 WD4 clears M on a write-through hit, even
            // following an unsafe mode change from copyback. Software must
            // clean before changing modes or untouched dirty bytes can be lost.
            if (pending_ == Pending::WriteThrough) lines_[pending_slot_].dirty = false;
            offset_ += pending_size_;
        }
        pending_ = Pending::None;
    }

private:
    struct Region { uint64_t base, size; };
    const Region* regionFor(uint64_t address) const {
        for (const auto& region : regions_)
            if (address >= region.base && address - region.base < region.size)
                return &region;
        return nullptr;
    }
    struct Line {
        std::array<uint8_t, LineBytes> bytes{};
        uint64_t address = 0;
        bool valid = false, dirty = false;
    };
    enum class Pending { None, Fill, BypassRead, BypassWrite, WriteThrough, Push, Evict };
    static constexpr uint32_t NoSlot = Sets * Ways;
    static uint32_t setIndex(uint64_t address) {
        return (address / LineBytes) & (Sets - 1);
    }
    uint32_t find(uint64_t line_address) const {
        const uint32_t first = setIndex(line_address) * Ways;
        for (uint32_t slot = first; slot < first + Ways; ++slot)
            if (lines_[slot].valid && lines_[slot].address == line_address) return slot;
        return NoSlot;
    }
    uint32_t victim(uint32_t set) const {
        const uint32_t first_way = (cacr_ & DHLCK) ? 2 : 0;
        for (uint32_t way = first_way; way < Ways; ++way)
            if (!lines_[set * Ways + way].valid) return set * Ways + way;
        const uint32_t way = first_way ? 2 + (replacement_ >> 1) : replacement_;
        return set * Ways + way;
    }
    void start() {
        if (active_) throw std::logic_error("overlapping window cache transactions");
        active_ = true; pending_ = Pending::None; pushing_ = false;
        count_ = offset_ = 0; bytes_.clear(); write_ = false;
    }
    uint32_t mode(uint64_t address) const {
        if (!(cacr_ & DEC)) return 2;
        for (unsigned i = 0; i < 2; ++i) { // ACR2/3 control instructions only
            const uint32_t acr = acr_[i];
            const uint32_t sm = (acr >> 13) & 3;
            if (!(acr & 0x8000u) || sm == 0) continue;
            if (acr & 0x400u) { // AMM: top byte exact, 1-MiB granularity below it
                if ((address >> 24) == (acr >> 24) &&
                    ((((address >> 20) ^ (acr >> 20)) & ~(acr >> 16) & 15) == 0))
                    return (acr >> 5) & 3;
                continue;
            }
            const uint32_t mask = (acr >> 16) & 0xff;
            if ((((address >> 24) ^ (acr >> 24)) & ~mask & 0xff) == 0)
                return (acr >> 5) & 3;
        }
        return (cacr_ >> 25) & 3;
    }
    std::vector<Region> regions_;
    uint32_t cacr_ = 0;
    std::array<uint32_t, 4> acr_{};
    std::array<Line, Sets * Ways> lines_{};
    uint32_t replacement_ = 0, pending_slot_ = 0;
    uint64_t dirty_discards_ = 0;
    bool active_ = false, pushing_ = false, invalidate_push_ = false, write_ = false;
    Pending pending_ = Pending::None;
    uint64_t address_ = 0, pending_address_ = 0;
    uint32_t count_ = 0, offset_ = 0, pending_size_ = 0;
    std::vector<uint8_t> bytes_;
};

} }
#endif
