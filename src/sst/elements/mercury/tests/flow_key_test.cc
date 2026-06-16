// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.
//
// Portions are copyright of other developers:
// See the file CONTRIBUTORS.TXT in the top level directory
// of the distribution for more information.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

// Regression for the NIC reassembly-key fix (components/nic.cc): flowId is only
// unique per sender, so two senders reusing one flowId into the same NIC must not
// co-mingle in the completion queue. recvCqFlowKey() folds the source in to keep
// each in-flight message's reassembly slot private.
//
// Dependency-free (just <mercury/hardware/common/flow_key.h>), so it builds and
// runs without the full SST stack:
//   c++ -std=c++17 -I src/sst/elements flow_key_test.cc -o flow_key_test && ./flow_key_test

#include <mercury/hardware/common/flow_key.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>

using SST::Hg::recvCqFlowKey;
using SST::Hg::kFlowKeyMaxSrc;

namespace {

// A minimal stand-in for RecvCQ's byte accounting: accumulate per key and report
// completion when the arrived bytes reach the message total.
struct MiniCQ {
  struct Slot { uint64_t arrived = 0; uint64_t total = 0; };
  std::map<uint64_t, Slot> slots;

  // Returns true exactly when this packet completes its message. Aborts (via the
  // overshoot assert) if accounting ever exceeds the total -- the real NIC turns
  // that into the "couldn't get a flow" fatal.
  bool recv(uint64_t key, uint64_t bytes, uint64_t total) {
    auto& s = slots[key];
    s.total = total;
    s.arrived += bytes;
    assert(s.arrived <= s.total && "reassembly overshoot -- keys co-mingled");
    return s.arrived == s.total;
  }
};

// Replays the failing scenario: two senders (src 0 and src 1) each send a
// 3-packet, 300-byte message that happens to share flowId 57, packets
// interleaved as they would arrive at one NIC under heavy fan-in.
void test_collision_resolved_by_source() {
  MiniCQ cq;
  const uint64_t flowId = 57;
  const uint64_t total = 300, pkt = 100;
  const uint64_t kA = recvCqFlowKey(0, flowId);
  const uint64_t kB = recvCqFlowKey(1, flowId);

  assert(kA != kB && "same flowId from different sources must not share a key");

  int completedA = 0, completedB = 0;
  // Interleave: A, B, A, B, A(tail), B(tail).
  completedA += cq.recv(kA, pkt, total);
  completedB += cq.recv(kB, pkt, total);
  completedA += cq.recv(kA, pkt, total);
  completedB += cq.recv(kB, pkt, total);
  completedA += cq.recv(kA, pkt, total);
  completedB += cq.recv(kB, pkt, total);

  assert(completedA == 1 && "sender A's message completed exactly once");
  assert(completedB == 1 && "sender B's message completed exactly once");
}

// Guards the layout: same (src, flowId) is stable; flowId still distinguishes
// messages from one sender; the source occupies the high bits.
void test_key_layout() {
  assert(recvCqFlowKey(3, 57) == recvCqFlowKey(3, 57));
  assert(recvCqFlowKey(0, 1) != recvCqFlowKey(0, 2));
  assert(recvCqFlowKey(1, 0) == (uint64_t(1) << 48));
  assert((recvCqFlowKey(kFlowKeyMaxSrc, 0) >> 48) == kFlowKeyMaxSrc);
}

// Sanity that the old, broken keying (flowId alone) would have co-mingled --
// documents what the fix prevents. Here a shared slot overshoots its total.
void test_old_keying_would_overshoot() {
  MiniCQ cq;
  const uint64_t total = 300, pkt = 100;
  // Both senders key by flowId alone -> same slot. After 4 of the 6 packets the
  // shared slot has 400 > 300: the overshoot the real NIC aborted on.
  bool overshoot = false;
  auto& s = cq.slots[57];
  for (int i = 0; i < 4; ++i) { s.total = total; s.arrived += pkt; }
  overshoot = s.arrived > s.total;
  assert(overshoot && "flowId-only keying co-mingles two senders (the bug)");
}

} // namespace

int main() {
  test_collision_resolved_by_source();
  test_key_layout();
  test_old_keying_would_overshoot();
  printf("flow_key_test: OK\n");
  return 0;
}
