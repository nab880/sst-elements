#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../quetz_window_cache.h"

using SST::Quetz::WindowDataCache;
using Bytes = std::vector<uint8_t>;
using Kind = WindowDataCache::Kind;

namespace {
constexpr uint64_t Base = 0x71000000;
constexpr uint32_t Copyback = WindowDataCache::DEC | (1u << 25);
constexpr uint16_t PushData = 0xf468;
constexpr uint16_t PushInstruction = 0xf4a8;

struct Memory {
    WindowDataCache cache;
    Bytes backing = Bytes(64);
    std::vector<WindowDataCache::Action> requests;

    Memory() {
        cache.configure(Base, backing.size());
        for (size_t i = 0; i < backing.size(); ++i) backing[i] = uint8_t(i);
    }
    Bytes dmaRead(size_t offset, size_t size) const {
        return {backing.begin() + offset, backing.begin() + offset + size};
    }
    void dmaWrite(size_t offset, const Bytes& bytes) {
        std::copy(bytes.begin(), bytes.end(), backing.begin() + offset);
    }
    void respond(const WindowDataCache::Action& action) {
        REQUIRE(action.address >= Base);
        REQUIRE(action.address + action.size <= Base + backing.size());
        const auto offset = size_t(action.address - Base);
        requests.push_back(action);
        if (action.kind == Kind::Read) {
            cache.complete(dmaRead(offset, action.size));
        } else {
            REQUIRE(action.kind == Kind::Write);
            REQUIRE(action.bytes.size() == action.size);
            dmaWrite(offset, action.bytes);
            cache.complete();
        }
    }
    Bytes drain() {
        for (unsigned i = 0; i < 100; ++i) {
            auto action = cache.next();
            if (action.kind == Kind::Done) {
                CHECK_FALSE(cache.active());
                return action.bytes;
            }
            CHECK(cache.active());
            respond(action);
        }
        throw std::runtime_error("cache did not finish a bounded transaction");
    }
    Bytes read(size_t offset, unsigned size) {
        cache.access(Base + offset, size, false);
        return drain();
    }
    void write(size_t offset, const Bytes& bytes) {
        cache.access(Base + offset, bytes.size(), true, bytes);
        CHECK(drain().empty());
    }
    void control(uint32_t reg, uint32_t value) {
        cache.movec(reg, value);
        CHECK(drain().empty());
    }
    void push(uint16_t instruction = PushData) {
        cache.push(instruction);
        CHECK(drain().empty());
    }
};

uint32_t acr(uint8_t top, unsigned mode, unsigned supervisor = 1,
             uint8_t mask = 0) {
    return (uint32_t(top) << 24) | (uint32_t(mask) << 16) | 0x8000u |
           (supervisor << 13) | (mode << 5);
}
}

TEST_CASE("disabled cache keeps CPU and DMA byte views coherent") {
    Memory m;
    m.write(3, {0x91, 0x82, 0x73, 0x64});
    CHECK(m.dmaRead(3, 4) == Bytes({0x91, 0x82, 0x73, 0x64}));
    CHECK(m.read(3, 4) == m.dmaRead(3, 4));
    m.dmaWrite(3, {0x12, 0x34, 0x56, 0x78});
    CHECK(m.read(3, 4) == Bytes({0x12, 0x34, 0x56, 0x78}));
}

TEST_CASE("copyback leaves stale DMA bytes until actual push writes complete") {
    Memory m;
    m.control(2, Copyback);
    const auto original = m.backing;
    m.write(4, {0xfe, 0xdc, 0xba, 0x98});
    CHECK(m.read(4, 4) == Bytes({0xfe, 0xdc, 0xba, 0x98}));
    CHECK(m.backing == original);
    m.push();
    CHECK(m.dmaRead(4, 4) == Bytes({0xfe, 0xdc, 0xba, 0x98}));
    CHECK(m.dmaRead(0, 4) == Bytes({0, 1, 2, 3}));
    CHECK(m.dmaRead(8, 8) == Bytes({8, 9, 10, 11, 12, 13, 14, 15}));
    m.dmaWrite(4, {1, 3, 5, 7});
    CHECK(m.read(4, 4) == Bytes({1, 3, 5, 7}));
}

TEST_CASE("DMA writeback stays stale to CPU until DCINVA") {
    Memory m;
    m.control(2, Copyback);
    CHECK(m.read(0, 4) == Bytes({0, 1, 2, 3}));
    m.dmaWrite(0, {0xa1, 0xb2, 0xc3, 0xd4});
    CHECK(m.read(0, 4) == Bytes({0, 1, 2, 3}));
    m.control(2, Copyback | WindowDataCache::DCINVA);
    CHECK(m.read(0, 4) == Bytes({0xa1, 0xb2, 0xc3, 0xd4}));
    CHECK(m.cache.dirtyDiscards() == 0);
}

TEST_CASE("DCINVA discards dirty input without issuing writeback") {
    Memory m;
    m.control(2, Copyback);
    const auto original = m.backing;
    m.write(0, {9, 9, 9, 9});
    m.write(20, {8, 8, 8, 8});
    m.requests.clear();
    m.control(2, Copyback | WindowDataCache::DCINVA);
    CHECK(m.requests.empty());
    CHECK(m.backing == original);
    CHECK(m.cache.dirtyDiscards() == 2);
    CHECK(m.read(0, 4) == Bytes({0, 1, 2, 3}));
    m.control(2, Copyback | WindowDataCache::DCINVA);
    CHECK(m.cache.dirtyDiscards() == 2);
}

TEST_CASE("DDPI push cleans dirty bytes while preserving valid cached data") {
    Memory m;
    m.control(2, Copyback | WindowDataCache::DDPI);
    m.write(0, {9, 8, 7, 6});
    m.push();
    CHECK(m.dmaRead(0, 4) == Bytes({9, 8, 7, 6}));
    m.dmaWrite(0, {4, 3, 2, 1});
    CHECK(m.read(0, 4) == Bytes({9, 8, 7, 6}));
    m.requests.clear();
    m.push();
    CHECK(m.requests.empty());
    CHECK(m.dmaRead(0, 4) == Bytes({4, 3, 2, 1}));
    m.control(2, Copyback | WindowDataCache::DCINVA);
    CHECK(m.read(0, 4) == Bytes({4, 3, 2, 1}));
    CHECK(m.cache.dirtyDiscards() == 0);
}

TEST_CASE("write-through misses do not allocate and hits retain cached data") {
    Memory m;
    m.control(2, WindowDataCache::DEC);
    m.write(7, {0x12, 0x34});
    CHECK(m.dmaRead(7, 2) == Bytes({0x12, 0x34}));
    REQUIRE(m.requests.size() == 1);
    CHECK(m.requests[0].kind == Kind::Write);
    m.dmaWrite(7, {0xab, 0xcd});
    CHECK(m.read(7, 2) == Bytes({0xab, 0xcd}));
    m.requests.clear();
    m.write(7, {0x56, 0x78});
    REQUIRE(m.requests.size() == 1);
    CHECK(m.requests[0].kind == Kind::Write);
    CHECK(m.dmaRead(7, 2) == Bytes({0x56, 0x78}));
    m.dmaWrite(7, {0x90, 0xef});
    CHECK(m.read(7, 2) == Bytes({0x56, 0x78}));
    m.control(2, WindowDataCache::DEC | WindowDataCache::DCINVA);
    CHECK(m.read(7, 2) == Bytes({0x90, 0xef}));
}

TEST_CASE("supervisor ACR selection respects address mask and first-match priority") {
    Memory m;
    m.control(2, Copyback);
    m.control(4, acr(0x70, 2, 1, 1));
    m.control(5, acr(0x71, 1));
    m.write(0, {9});
    CHECK(m.dmaRead(0, 1) == Bytes({9}));
    m.control(4, acr(0x72, 2));
    m.write(1, {8});
    CHECK(m.dmaRead(1, 1) == Bytes({1}));
    m.push();
    CHECK(m.dmaRead(1, 1) == Bytes({8}));
}

TEST_CASE("user-only and disabled ACRs fall through to supervisor default") {
    for (const auto region : {acr(0x71, 2, 0), acr(0x71, 2) & ~0x8000u}) {
        Memory m;
        m.control(2, Copyback);
        m.control(4, region);
        m.write(0, {9});
        CHECK(m.dmaRead(0, 1) == Bytes({0}));
    }
    for (unsigned supervisor : {2u, 3u}) {
        Memory m;
        m.control(2, Copyback);
        m.control(4, acr(0x71, 2, supervisor));
        m.write(0, {9});
        CHECK(m.dmaRead(0, 1) == Bytes({9}));
    }
}

TEST_CASE("instruction cache controls cannot clean or invalidate data") {
    Memory m;
    m.control(2, Copyback);
    m.control(6, acr(0x71, 2));
    m.control(7, acr(0x71, 2));
    m.write(0, {9});
    m.push(PushInstruction);
    CHECK(m.dmaRead(0, 1) == Bytes({0}));
    m.dmaWrite(0, {7});
    CHECK(m.read(0, 1) == Bytes({9}));
    m.push(0xf4e8);
    CHECK(m.dmaRead(0, 1) == Bytes({9}));
}

TEST_CASE("cross-line copyback preserves raw byte order and untouched bytes") {
    Memory m;
    m.control(2, Copyback);
    const Bytes payload = {0xde, 0xad, 0xbe, 0xef, 0x01, 0x23, 0x45, 0x67};
    m.write(13, payload);
    CHECK(m.read(13, 8) == payload);
    CHECK(m.dmaRead(13, 8) == Bytes({13, 14, 15, 16, 17, 18, 19, 20}));
    m.push();
    CHECK(m.dmaRead(13, 8) == payload);
    CHECK(m.dmaRead(11, 2) == Bytes({11, 12}));
    CHECK(m.dmaRead(21, 3) == Bytes({21, 22, 23}));
    REQUIRE(m.requests.size() == 4);
    CHECK(m.requests[0].kind == Kind::Read);
    CHECK(m.requests[0].address == Base);
    CHECK(m.requests[1].address == Base + 16);
    CHECK(m.requests[2].kind == Kind::Write);
    CHECK(m.requests[2].address == Base);
    CHECK(m.requests[3].address == Base + 16);
}

TEST_CASE("maintenance remains active until every backing write acknowledges") {
    Memory m;
    m.control(2, Copyback);
    m.write(0, {0xf1});
    m.write(32, {0xf2});
    m.cache.push(PushData);
    const auto first = m.cache.next();
    REQUIRE(first.kind == Kind::Write);
    CHECK(m.cache.active());
    CHECK_THROWS_AS(m.cache.next(), std::logic_error);
    CHECK_THROWS_AS(m.cache.access(Base, 1, false), std::logic_error);
    CHECK(m.dmaRead(0, 1) == Bytes({0}));
    m.respond(first);
    const auto second = m.cache.next();
    REQUIRE(second.kind == Kind::Write);
    CHECK(second.address == Base + 32);
    CHECK(m.cache.active());
    CHECK(m.dmaRead(32, 1) == Bytes({32}));
    m.respond(second);
    CHECK(m.cache.active());
    CHECK(m.cache.next().kind == Kind::Done);
    CHECK_FALSE(m.cache.active());
    CHECK(m.dmaRead(0, 1) == Bytes({0xf1}));
    CHECK(m.dmaRead(32, 1) == Bytes({0xf2}));
    CHECK_THROWS_AS(m.cache.complete(), std::logic_error);
    CHECK_THROWS_AS(m.cache.next(), std::logic_error);
}

TEST_CASE("line fills require one complete response before CPU read completes") {
    Memory m;
    m.control(2, Copyback);
    m.cache.access(Base + 1, 2, false);
    CHECK_THROWS_AS(m.cache.complete(), std::logic_error);
    const auto fill = m.cache.next();
    REQUIRE(fill.kind == Kind::Read);
    CHECK(fill.size == 16);
    CHECK_THROWS_AS(m.cache.next(), std::logic_error);
    CHECK_THROWS_AS(m.cache.complete({1, 2}), std::runtime_error);
    m.respond(fill);
    CHECK(m.cache.next().bytes == Bytes({1, 2}));
}

TEST_CASE("window and transaction validation reject unsupported geometry and accesses") {
    WindowDataCache cache;
    for (auto size : {uint64_t(0), uint64_t(15), uint64_t(1024 * 1024 + 16)})
        CHECK_THROWS_AS(cache.configure(Base, size), std::invalid_argument);
    CHECK_THROWS_AS(cache.configure(Base + 1, 64), std::invalid_argument);
    CHECK_THROWS_AS(cache.configure(0xfffffff0, 32), std::invalid_argument);
    CHECK_THROWS_AS(cache.configure(uint64_t(1) << 32, 16), std::invalid_argument);
    for (const auto address : {Base - 1, Base + 64}) {
        Memory m;
        CHECK_THROWS_AS(m.cache.access(address, 1, false), std::invalid_argument);
    }
    for (unsigned size : {0u, 9u}) {
        Memory m;
        CHECK_THROWS_AS(m.cache.access(Base, size, false), std::invalid_argument);
    }
    { Memory m; CHECK_THROWS_AS(m.cache.access(Base + 63, 2, false), std::invalid_argument); }
    { Memory m; CHECK_THROWS_AS(m.cache.access(Base, 2, true, {1}), std::invalid_argument); }
    { Memory m; CHECK_THROWS_AS(m.cache.movec(3, 0), std::invalid_argument); }
    { Memory m; CHECK_THROWS_AS(m.cache.movec(2, 0x40000000), std::invalid_argument); }
    { Memory m; CHECK_THROWS_AS(m.cache.push(0xf448), std::invalid_argument); }
}

TEST_CASE("rejected transactions preserve reusable cache state and dirty bytes") {
    Memory m;
    m.control(2, Copyback);
    m.write(0, {9, 8, 7, 6});
    CHECK_THROWS_AS(m.cache.access(Base + 63, 2, false), std::invalid_argument);
    CHECK_FALSE(m.cache.active());
    CHECK_THROWS_AS(m.cache.movec(3, 0), std::invalid_argument);
    CHECK_FALSE(m.cache.active());
    CHECK_THROWS_AS(m.cache.movec(2, 0x40000000 | WindowDataCache::DCINVA), std::invalid_argument);
    CHECK_FALSE(m.cache.active());
    CHECK_THROWS_AS(m.cache.push(0xf448), std::invalid_argument);
    CHECK_FALSE(m.cache.active());
    CHECK(m.cache.dirtyDiscards() == 0);
    CHECK(m.read(0, 4) == Bytes({9, 8, 7, 6}));
    CHECK(m.dmaRead(0, 4) == Bytes({0, 1, 2, 3}));
    m.push();
    CHECK(m.dmaRead(0, 4) == Bytes({9, 8, 7, 6}));
}
