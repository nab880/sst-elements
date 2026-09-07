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

The Mask-MPI and Ember regressions are the executable endpoint examples:

```sh
sst src/sst/elements/mask-mpi/tests/test_allreduce_innetwork.py
sst src/sst/elements/ember/tests/ember_allreduce_innetwork.py \
    --model-options="supported"
```

Four participants contribute `1.0` through `4.0`; each must receive `10.0`.
The six-edge static tree sends one reduction packet up and one result packet
down each edge, exactly `2E = 12` collective packets. Unsupported operations
exercise each endpoint stack's existing software fallback.

Installing a dormant service must not change ordinary Merlin traffic. The
default remains LRU when a processor is installed.

An installed collective processor requires LRU (`merlin.xbar_arb_lru`), even
when no invocation is running. RR rejects processors that own VNs during
initialization. Ordinary RR traffic and transparent pass processors that own
no VNs retain the ordinary arbitration path.

The regression compares disabled and dormant collective runs byte-for-byte
for the default and explicit LRU policy. Separate generic-service regressions
compare disabled/pass RR runs exactly:

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
models do not hand-write router ports, and `job.useCollectivePlan(plan)` binds
an Ember or Mercury job's NICs to the plan after allocation. Only allocated
participants and their paths are included. Binding requires exact physical
membership and logical rank order; construction checks them again to catch
reallocation. Sparse allocations retain physical host port numbers:

```python
system.allocateNodes(job, "linear")
plan = StaticCollectivePlan.from_fattree(topology, logical_ids=job, reduce_vn=1, result_vn=2)
job.useCollectivePlan(plan)
topology.router = StaticCollectiveRouter(plan)
```

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
the baseline model from a checkpoint. Mercury and Firefly are not
checkpointable.

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
output credits. Lazy emission needs only one temporary packet, irrespective
of fanout.

The service describes packet size in bits. Merlin derives flits from its
configured `flit_size`; the scalar profile does not require 8-byte flits.

Firefly stages contributions through its existing NIC DMA read resources and
completes results after a DMA write. Functional source bytes are copied on
acceptance, so later source changes do not affect the result. Simulated
source/result addresses come from the native Firefly operation.
`collectiveSubmitDelay_ns` and `collectiveCompletionDelay_ns` add nonnegative
engine overhead (both default to zero). `collectiveDmaReadBytes` and
`collectiveDmaWriteBytes` expose the memory traffic. The configured memory
model controls latency and contention; the basic model can approximate a
small buffered transfer as zero delay.

DMA write completion follows the selected native model. Firefly's
SimpleMemoryModel posts stores: the collective may complete after the write
enters the memory path, while its configured write latency continues to occupy
memory resources and can delay later operations. It does not wait for DRAM
retirement. This matches ordinary Firefly DMA writes.

Mercury `NodeCL` stages both contributions and results through its existing
MemoryModel channels, sharing bandwidth and contention with compute memory
traffic. Other Mercury node types retain their existing zero-cost memory
approximation. NIC parameters `collective_submit_delay_ns` and
`collective_completion_delay_ns` add nonnegative launch and completion
overheads (both default to zero). The adapter retains source snapshots and
incoming result bytes through memory completion and any network-credit wait.
The caller keeps its result buffer alive until completion; only one invocation
can be active.
