// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.

#include <iris/sumi/sumi.h>
#include <iris/sumi/sim_transport.h>

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace SST::Iris::sumi;

// Give the standalone test app a transport instance under the name expected by
// the public comm_* wrappers, without depending on Mask-MPI.
class SumiCollectiveTestTransport : public SimTransport
{
 public:
  SST_ELI_REGISTER_DERIVED(
    SST::Hg::Library,
    SumiCollectiveTestTransport,
    "sumicollectives",
    "sumi",
    SST_ELI_ELEMENT_VERSION(1, 0, 0),
    "SUMI collective test transport")

  SumiCollectiveTestTransport(SST::Params& params, SST::Hg::App* parent) :
    SimTransport(params, parent)
  {
  }
};

static CollectiveDoneMessage*
waitFor(CollectiveDoneMessage* msg)
{
  return msg ? msg : sumi_engine()->blockUntilNext(Message::default_cq);
}

int
runSumiCollectives()
{
  comm_init();
  const int rank = comm_rank();
  const int nproc = comm_nproc();
  const int count = 3;
  int errors = 0;

  // Give every root stable source storage for the full test run.
  std::vector<std::vector<int>> scatter_srcs(
      nproc, std::vector<int>(nproc * count));
  for (int root = 0; root < nproc; ++root){
    auto& src = scatter_srcs[root];
    for (int block = 0; block < nproc; ++block){
      for (int i = 0; i < count; ++i){
        src[block * count + i] = 1000 * root + 100 * block + i;
      }
    }
  }
  std::vector<int> reduce_src(nproc * count);
  std::vector<int> reduce_in_place(nproc * count);
  for (int block = 0; block < nproc; ++block){
    for (int i = 0; i < count; ++i){
      int value = (rank + 1) * (block + 1) + i;
      reduce_src[block * count + i] = value;
      reduce_in_place[block * count + i] = value;
    }
  }

  // Exercise every root, including the nonzero even roots that must forward
  // from their received subtree rather than the caller's full source buffer.
  std::vector<int> scatter_dst(count);
  for (int root = 0; root < nproc; ++root){
    auto& src = scatter_srcs[root];
    std::fill(scatter_dst.begin(), scatter_dst.end(), -1);
    auto* done = waitFor(comm_scatter(root, scatter_dst.data(), src.data(), count,
                                      sizeof(int), 100 + root,
                                      Message::default_cq));
    for (int i = 0; i < count; ++i){
      int expected = 1000 * root + 100 * rank + i;
      if (scatter_dst[i] != expected){
        std::printf("Rank %d FAIL scatter root=%d index=%d got=%d expected=%d\n",
                    rank, root, i, scatter_dst[i], expected);
        ++errors;
      }
    }
    if (done->result() != scatter_dst.data() || done->comm_rank() != rank){
      std::printf("Rank %d FAIL scatter completion root=%d\n", rank, root);
      ++errors;
    }
    delete done;
  }

  auto checkReduceResult = [&](const int* dst, const char* mode){
    int rank_sum = nproc * (nproc + 1) / 2;
    for (int i = 0; i < count; ++i){
      int expected = (rank + 1) * rank_sum + nproc * i;
      if (dst[i] != expected){
        std::printf("Rank %d FAIL reduce-scatter %s index=%d got=%d expected=%d\n",
                    rank, mode, i, dst[i], expected);
        ++errors;
      }
    }
  };

  std::vector<int> reduce_dst(count, -1);
  auto* done = waitFor(comm_reduce_scatter<int, Add>(
      reduce_dst.data(), reduce_src.data(), count, 200,
      Message::default_cq));
  checkReduceResult(reduce_dst.data(), "separate");
  if (done->result() != reduce_dst.data() || done->comm_rank() != rank){
    std::printf("Rank %d FAIL reduce-scatter completion\n", rank);
    ++errors;
  }
  delete done;

  // The actor must tolerate the result aliasing the start of the full input.
  done = waitFor(comm_reduce_scatter<int, Add>(
      reduce_in_place.data(), reduce_in_place.data(), count, 201,
      Message::default_cq));
  checkReduceResult(reduce_in_place.data(), "in-place");
  if (done->result() != reduce_in_place.data() || done->comm_rank() != rank){
    std::printf("Rank %d FAIL in-place reduce-scatter completion\n", rank);
    ++errors;
  }
  delete done;

  // Payload-free modeling must also work for eager and rendezvous protocols.
  done = waitFor(comm_reduce_scatter<int, Add>(
      nullptr, nullptr, count, 202, Message::default_cq));
  if (done->result() != nullptr || done->comm_rank() != rank){
    std::printf("Rank %d FAIL null-buffer completion\n", rank);
    ++errors;
  }
  delete done;

  std::printf("Rank %d %s\n", rank, errors ? "FAIL" : "PASS");
  comm_finalize();
  return errors ? 1 : 0;
}
