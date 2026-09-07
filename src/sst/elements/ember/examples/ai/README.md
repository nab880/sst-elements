# In-network collectives for AI training

This example runs functional synchronous data-parallel SGD on eight Ember
workers. Each worker owns a different quadratic training sample, computes its
local gradient, uses `MPI_Allreduce`, and applies the same averaged update.
Every rank verifies both collective results and the final trained model.

The example selects LRU for both comparison paths. LRU is the supported
policy for installed collective processors. RR remains available for ordinary
traffic and pass-through processors that own no VNs.

Two scenarios expose the current capability boundary:

- `scalar` trains a one-parameter model. Its `SUM/F64/count=1` gradient is
  accepted by the static in-network collective.
- `hybrid` trains four parameters and also synchronizes a scalar loss. The
  vector gradient transparently uses Firefly's software tree, while the scalar
  loss uses the in-network tree.

Run a like-for-like comparison from the source tree after installing SST Core
and SST Elements:

```sh
python3 src/sst/elements/ember/examples/ai/compare.py
```

Or inspect one path directly:

```sh
sst src/sst/elements/ember/examples/ai/data_parallel_training.py \
    --model-options="scalar offload"
sst src/sst/elements/ember/examples/ai/data_parallel_training.py \
    --model-options="scalar software"
sst src/sst/elements/ember/examples/ai/data_parallel_training.py \
    --model-options="hybrid offload"
```

The model reports each worker's average simulated step and collective time;
the comparison driver uses the slowest worker. Merlin and Firefly statistics
prove which calls entered the service: an eight-rank scalar
reduction traverses the fourteen-edge physical tree once upward and once
downward, producing 28 service-tree transmissions per epoch. The software
baseline keeps the same topology, workload, VNs, and service installation; it
changes only `forceSoftware`.

This is a correctness-oriented architectural model, not a hardware performance
claim. The useful result is the controlled A/B comparison and selective
fallback behavior. Current limits remain one `SUM/F64` element, one static
route, one rank per NIC, and serialized blocking collectives.

The equivalent Mercury and Mask-MPI application is documented in
[`mask-mpi/examples/ai`](../../../mask-mpi/examples/ai/README.md).
