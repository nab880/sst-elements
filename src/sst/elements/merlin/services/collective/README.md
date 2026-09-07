# Static Merlin collective processor

This experimental processor performs one functional scalar `SUM/F64`
collective through Merlin's ordinary ports and crossbar. It owns two VNs and
accepts immutable, validated local tree projections. The generic router
models ingress transfer bandwidth, physical-input occupancy, and a bounded
aggregate synthetic-output queue. The processor models ordered additions and
result latency, constructing fanout packets only as output capacity permits.

The profile supports one static route and one active invocation per router.
It rejects unsupported signatures and retired invocations, and retains
results under output backpressure. The shared endpoint contract preserves
source ownership and publishes results before a reentrant completion callback.

Run the processor and endpoint contract fixtures with:

```sh
sst src/sst/elements/merlin/tests/merlin_static_processor_contract.py
sst src/sst/elements/merlin/tests/collective_contract.py
```

The next layer adds model-side tree compilation and transport validation.
