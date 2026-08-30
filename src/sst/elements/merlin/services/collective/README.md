# Static Merlin collective

This experimental network service performs one functional `SUM/F64` collective
through Merlin's ordinary ports and crossbar. It is a correctness model, not a
production collective implementation or throughput benchmark.

Models construct one `sst.merlin.collective.StaticCollectivePlan` from
router-to-router and endpoint-to-router links. The plan validates the complete
tree, unique ports, participant identities, route identity, VCs, and bounded
egress capacity before installing immutable local projections. Routers retain
init/setup checks for transport facts unavailable to the model, including
connectivity, flit size, output capacity, and downstream credits. Any failure
therefore ends initialization before timed traffic runs.

The Mask-MPI and Ember regressions are the executable endpoint examples:

```sh
sst src/sst/elements/mask-mpi/tests/test_allreduce_innetwork.py \
    --model-options="supported"
sst src/sst/elements/ember/tests/ember_allreduce_innetwork.py \
    --model-options="supported"
```

Four participants contribute `1.0` through `4.0`; each must receive `10.0`.
The six-edge static tree sends one reduction packet up and one result packet
down each edge, exactly `2E = 12` collective packets. Unsupported operations
exercise each endpoint stack's existing software fallback.

Installing a dormant service must not change ordinary Merlin traffic. The
regression compares these runs byte-for-byte:

```sh
sst src/sst/elements/merlin/tests/merlin_static_ordinary_baseline.py
sst src/sst/elements/merlin/tests/merlin_static_ordinary_baseline.py \
    --model-options="service"
```

Current limits are one static route, one active invocation per router, one
`F64` element, and one rank per NIC. There is no dynamic setup, failure
recovery, cancellation, nonblocking collective, batching, or throughput tuning.
