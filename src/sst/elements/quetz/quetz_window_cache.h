// Copyright (c) 2026, NTESS. All rights reserved.
#ifndef SST_QUETZ_WINDOW_CACHE_H
#define SST_QUETZ_WINDOW_CACHE_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <vector>

namespace SST { namespace Quetz {

// Opt-in functional ColdFire data cache for one SST-owned window. This is
// deliberately independent of the trace cache and QEMU RAM. No eviction or
// timing model: every 16-byte line in the bounded window may remain resident.
// The caller serializes guest transactions and acknowledges maintenance only
// after every returned backing-memory write has completed.
class WindowDataCache {
public:
    static constexpr uint32_t LineBytes = 16;
    static constexpr uint32_t DEC = 0x80000000u;
    static constexpr uint32_t DDPI = 0x10000000u;
    static constexpr uint32_t DCINVA = 0x01000000u;
    enum class Kind { Done, Read, Write };
    struct Action {
        Kind kind;
        uint64_t address;
        uint32_t size;
        std::vector<uint8_t> bytes;
    };

    void configure(uint64_t base, uint64_t size) {
        if (!size || size > 1024 * 1024 || base > UINT32_MAX ||
            size - 1 > UINT32_MAX - base || base % LineBytes || size % LineBytes)
            throw std::invalid_argument("window cache requires an aligned 16-byte to 1-MiB 32-bit window");
        base_ = base; size_ = size;
    }
    bool contains(uint64_t address) const {
        return address >= base_ && address - base_ < size_;
    }
    bool active() const { return active_; }
    uint64_t dirtyDiscards() const { return dirty_discards_; }

    void access(uint64_t address, uint32_t size, bool write,
                const std::vector<uint8_t>& bytes = {}) {
        if (!size || size > 8 || !contains(address) ||
            size > size_ - (address - base_) || (write && bytes.size() != size))
            throw std::invalid_argument("invalid window cache access");
        start();
        address_ = address; count_ = size; offset_ = 0; write_ = write;
        bytes_ = write ? bytes : std::vector<uint8_t>(size);
    }

    // MOVEC CACR/ACR0-3. Supervisor matching only; instruction-cache controls
    // are retained but have no data effect. Unsupported data-cache lock/fill
    // controls fail closed instead of silently claiming their semantics.
    void movec(uint32_t reg, uint32_t value) {
        if (reg != 2 && !(reg >= 4 && reg <= 7))
            throw std::invalid_argument("unsupported ColdFire cache control register");
        if (reg == 2 && (value & 0x68000000u))
            throw std::invalid_argument("unsupported CACR data-cache lock/fill controls");
        start();
        if (reg == 2) {
            if (value & DCINVA) {
                for (const auto& entry : lines_)
                    dirty_discards_ += entry.second.dirty;
                lines_.clear(); // invalidate discards, it does NOT write back
            }
            cacr_ = value & ~DCINVA;
        } else {
            acr_[reg - 4] = value;
        }
    }

    // CPUSHL is a set/way operation, not an address-range clean. This scoped
    // functional model approximates each data-selected operation as a full
    // window sweep. DDPI suppresses invalidation after the push.
    void push(uint16_t instruction) {
        if ((instruction & 0xff38u) != 0xf428u || !(instruction & 0xc0u))
            throw std::invalid_argument("invalid ColdFire CPUSHL instruction");
        start();
        if (!(instruction & 0x40u)) return; // instruction cache only
        pushing_ = true;
        invalidate_push_ = !(cacr_ & DDPI);
        for (const auto& entry : lines_) push_lines_.push_back(entry.first);
    }

    Action next() {
        if (!active_ || pending_ != Pending::None)
            throw std::logic_error("window cache transaction ordering error");
        if (pushing_) {
            while (push_index_ < push_lines_.size()) {
                const uint64_t address = push_lines_[push_index_];
                auto& line = lines_.at(address);
                if (line.dirty) {
                    pending_ = Pending::Push;
                    pending_address_ = address;
                    return {Kind::Write, address, LineBytes,
                            {line.bytes.begin(), line.bytes.end()}};
                }
                if (invalidate_push_) lines_.erase(address);
                ++push_index_;
            }
        } else {
            while (offset_ < count_) {
                const uint64_t address = address_ + offset_;
                const uint64_t line_address = address & ~(uint64_t(LineBytes) - 1);
                const uint32_t in_line = address - line_address;
                pending_size_ = std::min(count_ - offset_, LineBytes - in_line);
                pending_address_ = line_address;
                const uint32_t cache_mode = mode(address);
                if (cache_mode >= 2 || (write_ && cache_mode == 0 && lines_.find(line_address) == lines_.end())) {
                    pending_ = write_ ? Pending::BypassWrite : Pending::BypassRead;
                    return {write_ ? Kind::Write : Kind::Read, address, pending_size_,
                        write_ ? std::vector<uint8_t>(bytes_.begin() + offset_,
                            bytes_.begin() + offset_ + pending_size_) : std::vector<uint8_t>{}};
                }
                auto it = lines_.find(line_address);
                if (it == lines_.end()) {
                    pending_ = Pending::Fill;
                    return {Kind::Read, line_address, LineBytes, {}};
                }
                auto& line = it->second;
                if (write_) {
                    std::copy_n(bytes_.begin() + offset_, pending_size_,
                                line.bytes.begin() + in_line);
                    if (cache_mode == 0) { // write-through
                        pending_ = Pending::WriteThrough;
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
            lines_[pending_address_] = line;
        } else if (pending_ == Pending::BypassRead) {
            if (bytes.size() != pending_size_) throw std::runtime_error("short window cache read");
            std::copy(bytes.begin(), bytes.end(), bytes_.begin() + offset_);
            offset_ += pending_size_;
        } else if (pending_ == Pending::Push) {
            if (invalidate_push_) lines_.erase(pending_address_);
            else lines_.at(pending_address_).dirty = false;
            ++push_index_;
        } else {
            offset_ += pending_size_;
        }
        pending_ = Pending::None;
    }

private:
    struct Line { std::array<uint8_t, LineBytes> bytes{}; bool dirty = false; };
    enum class Pending { None, Fill, BypassRead, BypassWrite, WriteThrough, Push };
    void start() {
        if (active_) throw std::logic_error("overlapping window cache transactions");
        active_ = true; pending_ = Pending::None; pushing_ = false;
        count_ = offset_ = 0; bytes_.clear(); write_ = false;
        push_lines_.clear(); push_index_ = 0;
    }
    uint32_t mode(uint64_t address) const {
        if (!(cacr_ & DEC)) return 2;
        for (unsigned i = 0; i < 2; ++i) { // ACR2/3 control instructions only
            const uint32_t acr = acr_[i];
            const uint32_t sm = (acr >> 13) & 3;
            if (!(acr & 0x8000u) || sm == 0) continue;
            const uint32_t mask = (acr >> 16) & 0xff;
            if ((((address >> 24) ^ (acr >> 24)) & ~mask & 0xff) == 0)
                return (acr >> 5) & 3;
        }
        return (cacr_ >> 25) & 3;
    }
    uint64_t base_ = 0, size_ = 0;
    uint32_t cacr_ = 0;
    std::array<uint32_t, 4> acr_{};
    std::map<uint64_t, Line> lines_;
    uint64_t dirty_discards_ = 0;
    bool active_ = false, pushing_ = false, invalidate_push_ = false, write_ = false;
    Pending pending_ = Pending::None;
    uint64_t address_ = 0, pending_address_ = 0;
    uint32_t count_ = 0, offset_ = 0, pending_size_ = 0;
    std::vector<uint8_t> bytes_;
    std::vector<uint64_t> push_lines_;
    size_t push_index_ = 0;
};

} }
#endif
