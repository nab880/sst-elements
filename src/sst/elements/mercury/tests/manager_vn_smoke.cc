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

#define ssthg_app_name manager_vn_smoke

#include <cstdlib>
#include <iostream>

#include <mercury/components/node_base.h>
#include <mercury/components/operating_system_impl.h>
#include <mercury/hardware/network/network_message.h>
#include <mercury/operating_system/libraries/library.h>
#include <mercury/common/skeleton.h>

using namespace SST::Hg;

namespace {

constexpr const char* library_name = "manager_vn_smoke";

class ManagerVnSink : public Library
{
public:
  explicit ManagerVnSink(OperatingSystemAPI* os) :
      Library(library_name, SoftwareId(1, static_cast<int>(os->addr())), os),
      received_(false)
  {}

  ~ManagerVnSink() override
  {
    os_->unregisterEventLib(this);
  }

  void incomingRequest(Request* request) override
  {
    auto* message = dynamic_cast<NetworkMessage*>(request);
    const bool valid = message != nullptr && !received_ &&
        message->libname() == library_name && message->flowId() == 1 &&
        message->fromaddr() == 0 && message->toaddr() == 1 &&
        message->byteLength() == 8 &&
        message->type() == NetworkMessage::smsg_send;
    delete request;
    if (!valid) std::abort();
    received_ = true;
  }

  bool received() const { return received_; }

private:
  bool received_;
};

} // namespace

int main(int, char**)
{
  OperatingSystemAPI* os = OperatingSystemImpl::currentOs();
  ManagerVnSink sink(os);

  if (os->addr() == 0) {
    auto* message = new NetworkMessage(
        0, 1, library_name, 1, 1, 0, 8, false, nullptr,
        NetworkMessage::smsg{});
    message->putOnWire();
    os->node()->nic()->sendManagerMsg(message);
  }

  ssthg_nanosleep(100);

  if (os->addr() == 1) {
    if (!sink.received()) std::abort();
    std::cout << "manager_vn_smoke PASS\n";
  }
  else if (sink.received()) {
    std::abort();
  }

  return 0;
}
