// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.

#define ssthg_app_name collective_host_timing

#include <mercury/components/node_base.h>
#include <mercury/components/operating_system_impl.h>
#include <mercury/common/skeleton.h>
#include <merlin/services/collective/staticCollectiveEndpoint.h>

#include <cstdlib>
#include <cstdio>
#include <string>

using namespace SST::Hg;
using namespace SST::Collective;

namespace {

class HostTimingSink final : public CollectiveCompletionSink, public CollectiveReadySink
{
public:
  HostTimingSink(OperatingSystemAPI* os, bool reentrant) :
    os_(os), endpoint_(os->collectiveEndpoint()), reentrant_(reentrant)
  {
    if (!endpoint_ || !endpoint_->bind(*this, *this)) std::abort();
  }

  void submit(uint64_t invocation, bool contended)
  {
    invocation_ = invocation;
    completed_ = false;
    value_ = os_->addr() + 1.0;
    if (contended) {
      memory_done_ = false;
      os_->node()->accessHostMemory(400, newCallback(this, &HostTimingSink::memoryDone));
    }
    CollectiveSubmission submission;
    submission.invocation_id = invocation;
    submission.signature = STATIC_COLLECTIVE_SIGNATURE_V1;
    submission.source = {reinterpret_cast<const uint8_t*>(&value_), sizeof(value_)};
    submission.result = {reinterpret_cast<uint8_t*>(&value_), sizeof(value_)};
    started_at_ = os_->now();
    if (endpoint_->trySubmitCollective(submission) != CollectiveSubmitResult::Accepted) std::abort();
    value_ = -99.0; // Accepted owns its snapshot while modeled staging is pending.
    ++submission.invocation_id;
    if (endpoint_->trySubmitCollective(submission) != CollectiveSubmitResult::Retry || value_ != -99.0) std::abort();
  }

  void complete(uint64_t invocation, CollectiveCompletionStatus status) override
  {
    if (invocation != invocation_ || status != CollectiveCompletionStatus::Success || value_ != 3.0) std::abort();
    std::printf("Mercury collective host timing rank=%d invocation=%" PRIu64
                " elapsed_ps=%" PRIu64 " in_place=PASS\n",
        static_cast<int>(os_->addr()), invocation, (os_->now() - started_at_).ticks());
    completed_ = true;
    if (reentrant_ && invocation_ == 1) submit(2, false);
  }

  void ready() override { std::abort(); }
  void memoryDone() { memory_done_ = true; }
  bool completed() const { return completed_ && memory_done_; }

private:
  OperatingSystemAPI* os_;
  CollectiveEndpoint* endpoint_;
  Timestamp started_at_;
  double value_ = 0;
  uint64_t invocation_ = 0;
  bool completed_ = false;
  bool memory_done_ = true;
  bool reentrant_ = false;
};

} // namespace

int main(int argc, char** argv)
{
  const std::string mode = argc > 1 ? argv[1] : "baseline";
  const bool reentrant = mode == "reentrant";
  HostTimingSink sink(OperatingSystemImpl::currentOs(), reentrant);
  const bool contended = mode == "contended";
  for (uint64_t invocation = 1; invocation <= (reentrant ? 1 : 2); ++invocation) {
    sink.submit(invocation, contended);
    int waited = 0;
    while (!sink.completed() && waited++ < 10000) ssthg_nanosleep(1);
    if (!sink.completed()) std::abort();
    ssthg_nanosleep(1);
  }
  return 0;
}
