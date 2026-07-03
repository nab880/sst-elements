// Copyright 2009-2024 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2024, NTESS
// All rights reserved.
//
// Portions are copyright of other developers:
// See the file CONTRIBUTORS.TXT in the top level directory
// of the distribution for more information.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#ifndef CARCOSA_RING_PROTOCOL_H
#define CARCOSA_RING_PROTOCOL_H

#include <string>

namespace SST {
namespace Carcosa {

// ---------------------------------------------------------------------------
// Hali ring accelerator handshake.
//
// A compute "hub" (Hali hosting a workload driver) and an accelerator ring
// partner (a GPU model, a weight-reader, a balar bridge) exchange HaliEvents
// over the Hali left/right ring using this small, transport-neutral protocol:
//
//   dispatch   : hub sends  SeqLen  (getNum() = current sequence length),
//                then        Cmd     (getNum() = kernel/command index;
//                                      getPayload() = optional opaque kernel
//                                      descriptor the partner alone interprets)
//   completion : partner replies Done (getNum() = iteration/echo)
//   teardown   : hub sends  Exit
//
// carcosa never parses the Cmd payload; the hub's driver and the partner agree
// on its layout privately. Any accelerator agent (weight-reader, balar bridge)
// implements the same handshake so drivers are written once.
// ---------------------------------------------------------------------------
namespace RingTag {
    static constexpr const char* Cmd    = "cmd";     // dispatch a GPU-resident kernel
    static constexpr const char* SeqLen = "seqlen";  // sequence-length hint before a Cmd
    static constexpr const char* Done   = "done";    // partner completed the dispatched work
    static constexpr const char* Exit   = "exit";    // end of run
}

// Convenience predicates (avoid scattering string literals across agents).
inline bool ringTagIs(const std::string& s, const char* tag) { return s == tag; }

} // namespace Carcosa
} // namespace SST

#endif // CARCOSA_RING_PROTOCOL_H
