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

/**
 * quetz_command_buffer.h — per-vCPU command ring buffer access.
 */

#ifndef _SST_QUETZ_COMMAND_BUFFER_H
#define _SST_QUETZ_COMMAND_BUFFER_H

#include "quetz_ipc_types.h"

#include <sst/core/interprocess/tunneldef.h>

namespace SST {
namespace Quetz {

using QuetzTunnelBase =
    SST::Core::Interprocess::TunnelDef<QuetzSharedData, QuetzCommand>;

/**
 * Delegates command storage/retrieval to the TunnelDef base of QuetzTunnel.
 */
class QuetzCommandBuffer {
public:
    explicit QuetzCommandBuffer(QuetzTunnelBase* tunnel = nullptr) : tunnel_(tunnel) {}

    void bind(QuetzTunnelBase* tunnel) { tunnel_ = tunnel; }

    void write(size_t vcpu, const QuetzCommand& cmd) {
        tunnel_->writeMessage(vcpu, cmd);
    }

    bool readNB(size_t vcpu, QuetzCommand* out) {
        return tunnel_->readMessageNB(vcpu, out);
    }

    void clearBuffer(size_t vcpu) { tunnel_->clearBuffer(vcpu); }

private:
    QuetzTunnelBase* tunnel_;
};

} // namespace Quetz
} // namespace SST

#endif // _SST_QUETZ_COMMAND_BUFFER_H
