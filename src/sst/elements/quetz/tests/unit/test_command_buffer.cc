#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <cstring>
#include <vector>

#include "../../quetz_ipc_types.h"

using SST::Quetz::QuetzCommand;
using SST::Quetz::QuetzShmemCmd;

// Minimal ring buffer mirroring TunnelDef per-vCPU queue semantics.
class MiniRing {
public:
    explicit MiniRing(size_t cap) : cap_(cap), buf_(cap) {}

    bool write(const QuetzCommand& cmd) {
        if (count_ >= cap_)
            return false;
        buf_[tail_] = cmd;
        tail_ = (tail_ + 1) % cap_;
        count_++;
        return true;
    }

    bool readNB(QuetzCommand* out) {
        if (count_ == 0)
            return false;
        *out = buf_[head_];
        head_ = (head_ + 1) % cap_;
        count_--;
        return true;
    }

    size_t count() const { return count_; }

private:
    size_t              cap_;
    size_t              head_ = 0;
    size_t              tail_ = 0;
    size_t              count_ = 0;
    std::vector<QuetzCommand> buf_;
};

TEST_CASE("command ring round-trip") {
    MiniRing ring(4);
    QuetzCommand a{};
    a.cmd = QuetzShmemCmd::QUETZ_CMD_READ;
    a.size = 8;
    a.addr = 0x1000;
    CHECK(ring.write(a));
    QuetzCommand b{};
    CHECK(ring.readNB(&b));
    CHECK(b.cmd == QuetzShmemCmd::QUETZ_CMD_READ);
    CHECK(b.size == 8);
    CHECK(b.addr == 0x1000);
}

TEST_CASE("command ring back-pressure") {
    MiniRing ring(2);
    QuetzCommand c{};
    CHECK(ring.write(c));
    CHECK(ring.write(c));
    CHECK_FALSE(ring.write(c));
    CHECK(ring.count() == 2);
}

TEST_CASE("command ring wrap") {
    MiniRing ring(2);
    QuetzCommand c{};
    c.addr = 1;
    CHECK(ring.write(c));
    c.addr = 2;
    CHECK(ring.write(c));
    QuetzCommand out{};
    CHECK(ring.readNB(&out));
    CHECK(out.addr == 1);
    c.addr = 3;
    CHECK(ring.write(c));
    CHECK(ring.readNB(&out));
    CHECK(out.addr == 2);
    CHECK(ring.readNB(&out));
    CHECK(out.addr == 3);
}
