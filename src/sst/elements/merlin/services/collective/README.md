# Static Merlin collective

This experimental network service performs one functional `SUM/F64` collective
through Merlin's ordinary ports and crossbar. It provides functional scalar
results and configurable resource timing. The defaults are illustrative;
performance studies must choose parameters for the hardware being modeled.

Models construct one `sst.merlin.collective.StaticCollectivePlan` from a fat
tree's shared connectivity description, or explicit router/endpoint links.
The plan validates the complete
tree, unique ports, participant identities, route identity, VCs, and bounded
egress capacity before installing immutable local projections. Routers retain
init/setup checks for transport facts unavailable to the model, including
connectivity, flit size, output capacity, and downstream credits. Any failure
therefore ends initialization before timed traffic runs.

Installing a dormant service must not change ordinary Merlin traffic. The
default remains LRU when a processor is installed.

LRU (`merlin.xbar_arb_lru`) is the supported crossbar policy for active
collective offload. The main microbenchmarks and AI examples select it
explicitly for both offload and software runs. RR (`merlin.xbar_arb_rr`) is
experimental for active services: sustained traffic on multiple synthetic
VCs can starve. Ordinary traffic and dormant services retain RR support;
the intentional RR regressions and experiments remain available to study
that policy.

The regression compares disabled and dormant runs byte-for-byte for the
default and each explicit LRU/RR policy:

```sh
sst src/sst/elements/merlin/tests/merlin_static_ordinary_baseline.py
sst src/sst/elements/merlin/tests/merlin_static_ordinary_baseline.py \
    --model-options="service"
```

The processor owns the plan's `reduce_vn` and `result_vn` on every router
that hosts it: the crossbar never arbitrates the VCs of those VNs and every
head on them is offered to the processor. Ordinary traffic must use other
VNs; an untagged packet on an owned VN is a fatal error. The tests keep
ordinary traffic on VN 0 and give the plan VNs 1 and 2.
The service VNs must use identity VN mappings in LinkControl: their endpoint
and router VN numbers must agree. Remapped service VNs are rejected by
transport capability checks before offload acceptance. Ordinary VNs may
still be remapped.
`StaticCollectivePlan.from_fattree` derives the tree from a `topoFatTree`, so
models do not hand-write router ports. Passing an allocated job as
`logical_ids` includes only its participants and their paths.
`plan.validate_job(job)` requires exact physical membership and logical rank
order; native bindings in the following layers repeat that check at build
time to catch reallocation. Sparse allocations retain physical host ports.

Endpoint stacks see one `CollectiveParticipant` (route, physical and logical
IDs, the two VNs) and drive the endpoint with `CollectiveSubmission`,
`complete(invocation_id, status)`, and `ready()`; the wire descriptor, not
the in-process API, carries the version.

The wire descriptor (`CollectiveServiceData`) carries a signature, a chunk
index, and a byte payload; the static v1 profile accepts exactly one `F64`
`SUM` element in one chunk, and enforces that in the processor and endpoints
rather than in the schema.

Current limits are one static route, one active invocation per router, one
`F64` element, and one rank per NIC. There is no dynamic setup, failure
recovery, cancellation, nonblocking collective, batching, or chunk pipelining.
A Merlin-only model with the processor installed can be checkpointed while
no invocation is active; an active invocation or staged ingress transfer at
checkpoint time is rejected, and `testsuite_default_collective.py` restarts
the baseline model from a checkpoint.

Resource timing uses the router's existing clock:

| Parameter | Default | Meaning |
| --- | --- | --- |
| `network_service_ingress_width` | 1 | Maximum transfers started and packets committed per cycle |
| `network_service_ingress_flits_per_cycle` | 1 | Transfer bandwidth per physical input |
| `network_service_shared_ingress` | true | Transfers reserve the ordinary crossbar input; false models an independent read path |
| `network_service_output_queue_depth` | 8 in `hr_router`, 1 in a plan | Aggregate synthetic packet capacity across all output/VC queues |
| `reduction_ops_per_cycle` | 1 | Ordered additions per processor cycle; set on the plan |
| `reduction_latency_cycles` | 1 | Additional latency after the final addition; set on the plan |

A transfer of `F` flits at bandwidth `B` starts at cycle `t` and commits no
earlier than `t + ceil(F/B)`. The head and its credits remain in PortControl
until commit. Each port has at most one staged transfer. The processor
rechecks admission before consuming it; if it is now busy, the provisional
transfer is abandoned and a later attempt pays the transfer cost again,
allowing another owned VN to make progress.

After every branch arrives, the reducer performs `branches - 1` additions in
fixed branch order, followed by the configured result latency. A single
branch still advances on the next router tick. A scalar result and unsent
branch bits remain in the processor; packets are constructed as output
capacity becomes available. Independent output queues avoid a blocked
destination holding up other ready destinations. All packets still share one
synthetic crossbar input, with bandwidth determined by `xbar_bw` and normal
output credits. `pending_egress_capacity` is a positive compatibility setting;
lazy emission needs only one temporary packet, irrespective of fanout.

The service describes packet size in bits. Merlin derives flits from its
configured `flit_size`; the scalar profile does not require 8-byte flits.
