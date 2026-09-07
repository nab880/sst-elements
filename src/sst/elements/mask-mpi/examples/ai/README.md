# AI training on Mercury and Mask-MPI

This example runs the same functional synchronous data-parallel SGD workload
as Ember's AI demo, but as a Mercury skeleton application using ordinary
Mask-MPI calls. Each of eight workers computes a local quadratic gradient,
calls blocking `MPI_Allreduce`, and applies the averaged update. Every worker
verifies every reduction and the final trained model.

The example selects LRU for both comparison paths. LRU is the supported
policy for installed collective processors. RR remains available for ordinary
traffic and pass-through processors that own no VNs.

The executable lives with Mask-MPI because that is Mercury's MPI consumer;
keeping it here avoids making the lower-level Mercury element depend on
Mask-MPI.

Two scenarios expose the current capability boundary:

- `scalar` trains one parameter. Its `SUM/F64/count=1` gradient is accepted by
  Mercury's static collective endpoint.
- `hybrid` trains four parameters and synchronizes a scalar loss. Mask-MPI
  transparently runs the unsupported vector gradient in software, then
  offloads the supported scalar loss.

Run the controlled comparison after installing SST Core and SST Elements:

```sh
python3 src/sst/elements/mask-mpi/examples/ai/compare.py
```

Or inspect one path directly:

```sh
sst src/sst/elements/mask-mpi/examples/ai/data_parallel_training.py \
    --model-options="scalar offload"
sst src/sst/elements/mask-mpi/examples/ai/data_parallel_training.py \
    --model-options="scalar software"
sst src/sst/elements/mask-mpi/examples/ai/data_parallel_training.py \
    --model-options="hybrid offload"
```

Both sides retain the same Mercury nodes, Mask-MPI workload, topology,
installed service, route, and virtual-network assignments. The only A/B
switch is `app1.enable_collective_offload`. Merlin statistics provide path
evidence: one accepted eight-rank scalar reduction produces 28 service-tree
transmissions, while the software baseline produces none.

This is a correctness-oriented architectural model, not a hardware performance
claim. Current offload limits remain one `SUM/F64` element, one static route,
one rank per NIC, and one blocking collective at a time.
