#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../quetz_window_cache_bank.h"

using SST::Quetz::WindowCacheBank;
using SST::Quetz::WindowDataCache;
using Bytes = std::vector<uint8_t>;
using Kind = WindowDataCache::Kind;
constexpr uint32_t Base = 0x71000000;
constexpr uint32_t Copyback = WindowDataCache::DEC | (1u << 25);

struct SharedMemory {
    WindowCacheBank bank;
    Bytes bytes = Bytes(64, 0x11);
    uint64_t next_id = 1;

    SharedMemory() { bank.configure(2, Base, bytes.size()); }

    Bytes backing(uint64_t address, size_t size) const {
        REQUIRE(address >= Base);
        REQUIRE(size <= bytes.size() - (address - Base));
        return {bytes.begin() + address - Base, bytes.begin() + address - Base + size};
    }

    Bytes drain(uint32_t cpu) {
        for (unsigned step = 0; step < 100; ++step) {
            const auto action = bank.cache(cpu).next();
            if (action.kind == Kind::Done) return action.bytes;
            const bool read = action.kind == Kind::Read;
            const auto id = next_id++;
            bank.issued(cpu, id, read);
            if (read) bank.complete(id, cpu, true, backing(action.address, action.size));
            else {
                REQUIRE(action.bytes.size() == action.size);
                REQUIRE(action.address >= Base);
                REQUIRE(action.size <= bytes.size() - (action.address - Base));
                std::copy(action.bytes.begin(), action.bytes.end(), bytes.begin() + action.address - Base);
                bank.complete(id, cpu, false);
            }
        }
        throw std::runtime_error("cache transaction did not terminate");
    }

    void control(uint32_t cpu, uint32_t value) {
        bank.cache(cpu).movec(2, value);
        CHECK(drain(cpu).empty());
    }
    Bytes read(uint32_t cpu, size_t offset) {
        bank.cache(cpu).access(Base + offset, 4, false);
        return drain(cpu);
    }
    void write(uint32_t cpu, size_t offset, const Bytes& data) {
        bank.cache(cpu).access(Base + offset, data.size(), true, data);
        CHECK(drain(cpu).empty());
    }
    void pushAll(uint32_t cpu) {
        for (unsigned set = 0; set < WindowDataCache::Sets; ++set)
            for (unsigned way = 0; way < WindowDataCache::Ways; ++way) {
                bank.cache(cpu).push(0xf468, (set << 4) | way);
                CHECK(drain(cpu).empty());
            }
    }
};

TEST_CASE("private CPU cache lines require writer clean and reader invalidate") {
    SharedMemory m;
    m.control(0, Copyback); m.control(1, Copyback);
    const Bytes old(4, 0x11), update{0x12, 0x34, 0x56, 0x78};
    CHECK(m.read(1, 4) == old);
    m.write(0, 4, update);
    CHECK(m.read(0, 4) == update);
    CHECK(m.backing(Base + 4, 4) == old);
    CHECK(m.read(1, 4) == old);
    m.pushAll(0);
    CHECK(m.backing(Base + 4, 4) == update);
    CHECK(m.read(1, 4) == old);
    m.control(1, Copyback | WindowDataCache::DCINVA);
    CHECK(m.read(1, 4) == update);
    CHECK_FALSE(m.bank.active());
}

TEST_CASE("reversed backing responses retain CPU ownership and aggregate drain state") {
    SharedMemory m;
    m.control(0, Copyback); m.control(1, Copyback);
    m.bank.cache(0).access(Base, 4, false);
    m.bank.cache(1).access(Base, 4, false);
    const auto a = m.bank.cache(0).next();
    const auto b = m.bank.cache(1).next();
    REQUIRE(a.kind == Kind::Read); REQUIRE(b.kind == Kind::Read);
    m.bank.issued(0, 10, true); m.bank.issued(1, 20, true);
    CHECK(m.bank.pending(10)->vcpu == 0);
    CHECK(m.bank.pending(20)->vcpu == 1);
    CHECK(m.bank.pending(99) == nullptr);
    CHECK_THROWS(m.bank.issued(1, 10, true));
    CHECK_THROWS(m.bank.complete(10, 1, true, Bytes(a.size, 0xaa)));
    CHECK_THROWS(m.bank.complete(10, 0, false));
    CHECK_THROWS(m.bank.complete(99, 0, true, Bytes(a.size, 0xaa)));
    CHECK_THROWS(m.bank.resetCore(0));
    m.bank.complete(20, 1, true, Bytes(b.size, 0xbb));
    CHECK(m.drain(1) == Bytes(4, 0xbb));
    CHECK(m.bank.active());
    CHECK(m.bank.pending(20) == nullptr);
    m.bank.complete(10, 0, true, Bytes(a.size, 0xaa));
    CHECK(m.drain(0) == Bytes(4, 0xaa));
    CHECK_FALSE(m.bank.active());
    CHECK(m.read(0, 0) == Bytes(4, 0xaa));
    CHECK(m.read(1, 0) == Bytes(4, 0xbb));
}

TEST_CASE("reset disables only its CPU controls and preserves private resident data") {
    SharedMemory m;
    m.control(0, Copyback); m.control(1, Copyback);
    m.write(0, 0, Bytes(4, 0xaa));
    m.write(1, 16, Bytes(4, 0xbb));
    m.bank.resetCore(0);
    CHECK(m.read(0, 0) == Bytes(4, 0x11));
    CHECK(m.read(1, 16) == Bytes(4, 0xbb));
    m.control(0, Copyback);
    CHECK(m.read(0, 0) == Bytes(4, 0xaa));
    m.control(0, Copyback | WindowDataCache::DCINVA);
    CHECK(m.read(0, 0) == Bytes(4, 0x11));
    CHECK(m.read(1, 16) == Bytes(4, 0xbb));
}

TEST_CASE("cache bank rejects unavailable CPUs and reconfiguration while active") {
    WindowCacheBank bank;
    CHECK_THROWS(bank.configure(0, Base, 64));
    CHECK_THROWS(bank.configure(3, Base, 64));
    bank.configure(1, Base, 64);
    CHECK_THROWS(bank.cache(1));
    CHECK_THROWS(bank.issued(0, 1, true));
    bank.cache(0).access(Base, 4, false);
    CHECK(bank.active());
    CHECK_THROWS(bank.configure(2, Base, 64));
    CHECK_THROWS(bank.resetCore(0));
}

TEST_CASE("native RAM and asynchronous window actions share each CPU cache bank") {
    SharedMemory m;
    m.bank.addRegion(0x80000000, 65536);
    m.bank.addRegion(0x40000000, 65536);
    for (unsigned cpu = 0; cpu < 2; ++cpu) {
        CHECK(m.bank.cache(cpu).contains(0x80000000));
        CHECK(m.bank.cache(cpu).contains(0x4000ffff));
        m.control(cpu, Copyback);
    }
    m.bank.cache(0).access(0x80000004, 4, false);
    m.bank.cache(1).access(Base, 4, false);
    const auto native = m.bank.cache(0).next();
    const auto remote = m.bank.cache(1).next();
    REQUIRE(native.kind == Kind::Read);
    REQUIRE(native.address == 0x80000000);
    REQUIRE(remote.kind == Kind::Read);
    m.bank.issued(1, 50, true);
    Bytes local_line(native.size);
    for (size_t i = 0; i < local_line.size(); ++i) local_line[i] = i;
    m.bank.cache(0).complete(local_line);
    const auto done = m.bank.cache(0).next();
    CHECK(done.kind == Kind::Done);
    CHECK(done.bytes == Bytes({4, 5, 6, 7}));
    CHECK(m.bank.active());
    m.bank.complete(50, 1, true, Bytes(remote.size, 0xab));
    CHECK(m.drain(1) == Bytes(4, 0xab));
    CHECK_FALSE(m.bank.active());
    CHECK_THROWS(m.bank.cache(0).access(0x8000fffe, 4, false));
    CHECK_THROWS(m.bank.cache(1).access(0x40010000, 4, false));
}
