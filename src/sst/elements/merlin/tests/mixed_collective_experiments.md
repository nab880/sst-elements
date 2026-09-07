# Mixed ordinary/collective experiments

Run against an installation containing the collective stack, Ember, and Firefly:

```sh
python3 run_mixed_collective.py --sst /absolute/path/to/install/bin/sst --output-dir /tmp/mixed-collective
python3 test_mixed_collective_metrics.py
```

The full matrix has 20 deterministic, single-rank simulations. `--quick` runs 8.
Use a fresh output directory. Each case preserves its exact command, stdout,
stderr, statistic CSV, and Ember motif log. `results.json`, `results.csv`, and
`summary.md` contain the measurements and failed invariants. The runner needs
only Python's standard library; no new C++ component or rebuild is required.

The fat tree `4,1:2` has two four-host leaf routers and one root. Physical hosts
0 and 4 perform 100 sequential, verified scalar Allreduces. The other six hosts
send 64-byte ordinary VN0 packets to logical rank `(rank + 3) % 6`, so every
ordinary packet crosses the root. Reduction VN1 traffic therefore shares root
inputs with ordinary traffic; synthetic reduction/result packets also compete
for leaf uplinks and root outputs. Links and crossbars are 8 GB/s, flits are
8 bytes, link latency is 1 ns, and router pipeline latency is zero. Each scalar
service packet is 104 bytes after rounding to flits.

Per-source offered load is 0.1, 0.5, or 0.9 of an 8 GB/s link. Three ordinary
sources share each leaf uplink, making 0.5 and 0.9 oversubscribed even before
adding collectives. A separate zero-load active control uses idle endpoints
because `merlin.offered_load` cannot accept zero. This preserves connected ports
but changes the idle endpoint component type, so zero-load results are controls,
not exact ordinary-traffic baseline comparisons.

LRU runs absent-service and installed-but-idle baselines,
then active shared/private ingress cases. The highest load additionally varies
aggregate service queue capacity (1 to 2), router/NIC buffers (128 to 512 bytes),
and ingress bandwidth (1 to 4 flits/cycle). Other defaults stay fixed. This is a
bounded sensitivity matrix, not a full factorial design. The scalar singleton
protocol, one ingress admission/cycle, and one synthetic input remain in place.

Installed collective processors require LRU, including idle processors. The
matrix therefore uses LRU throughout. Ordinary RR and transparent pass-service
behavior remain covered by the generic Merlin baseline regressions.

Ordinary traffic runs for 200 simulated microseconds. Cumulative LinkControl
statistics are sampled every 5 microseconds. For each active case, the runner
selects only whole sample intervals inside the intersection of the two ranks'
logged Allreduce motif windows. Every ordinary endpoint must receive packets in
every selected interval. Every invocation must be accepted at both ranks, both
ranks must verify the scalar result, and router accept/synthetic counts must
match the known three-router plan. All work must complete before the ordinary
window ends; a simulation deadline and wall-clock timeout catch failure to
finish. Inactive baseline comparisons require exact equality of every ordinary
statistic record, including every sample, rather than a latency tolerance.

LinkControl latency measures actual network injection to receipt; it excludes
time waiting at the source. The separate `ordinary_scheduled_mean_latency_ns`
uses OfferedLoad's whole-run scheduled-send latency, which includes source
backlog. `ordinary_source_backlog` preserves its backlog indicator. Packet
throughput uses received bytes and decimal GB/s. Collective latency is the
reported per-invocation Ember latency minus configured per-iteration compute.
Per-endpoint counts and overlap samples remain available in JSON, so aggregate
throughput cannot conceal one endpoint failing to progress.

Positive samples establish observed progress during this finite workload. They
do not prove scheduler fairness, bound individual packet waiting time, or
isolate every credit-eligibility phase. This fixture also does not pause a result
receiver to force a blocked fanout branch; the generic router/arbiter contract
fixtures should cover that adversarial case. The shared-ingress comparison
includes admission's input reservation as well as crossbar/output contention.
