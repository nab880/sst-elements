# Collective review stack

The local stack folds the design corrections into their owning feature layers.
The supported profile remains one static route, one active invocation per router,
and one scalar SUM/F64 value. Bounded chunks remain deferred.
Installed collective processors require LRU, including while idle. RR supports
ordinary traffic and pass-through processors that own no VNs; active-service RR
is deferred. Service VNs require identity mappings. Unsupported operations fall
back before acceptance; recovery of an accepted invocation is out of scope.

SST Core review commit: `f54d79af27c8756b50dbb9e8eb356eb5dcbee4d9` (`coll-work-1a`).
All existing numbered Core aliases use this commit. Elements layers from 1b onward
pin it in `.github/sst-core-ref`; publish the Core commit to `nab880/sst-core`
before publishing dependent Elements branches for CI.

The Elements review stack starts at `coll-work-1b`, based directly on upstream
`devel` ancestor `53c19eb6a`. The accelerator prototype and its ownership fix
are excluded from the review ancestry. The historical Elements `coll-work-1a`
branch remains available outside this stack; Core `coll-work-1a` is unchanged.
Layer 2 introduces the complete generic service transport. Its ordinary MTU
default is restored to upstream `2kB`; the prototype's `8KB` override is removed.

| Elements branch | Review base | Scope |
| --- | --- | --- |
| `coll-work-1b` | `devel` ancestor `53c19eb6a` | Neutral header-only contract, shared endpoint ownership, paired CI |
| `coll-work-2` | `coll-work-1b` | Generic service transport, LRU, ingress resources, output queues, VN/VC capacity validation and flit accounting |
| `coll-work-3` | `coll-work-2` | Scalar processor, ordered reduction timing, lazy fanout |
| `coll-work-4` | `coll-work-3` | Shared fat-tree connectivity, sparse plans, validation and idle checkpoint tests |
| `coll-work-5a` | `coll-work-4` | Mercury named VNs and ordinary transport tests |
| `coll-work-5b` | `coll-work-5a` | Mercury/Mask-MPI offload, native memory timing and job binding |
| `coll-work-6` | `coll-work-5b` | Firefly/Ember offload, native DMA timing and sparse job tests |
| `coll-work-arm64` | `coll-work-6` | Existing ARM64 context correction, kept separate from AI demos |
| `coll-work-contention` | `coll-work-arm64` | LRU mixed-traffic experiment fixture, runner, metric tests and this guide |

The initial review ends at `coll-work-contention`. AI demos are preserved as the
separate follow-up `coll-work-8`, based on `coll-work-contention`; they are not
part of the initial implementation diff.

`coll-work-7` aliases `coll-work-6`: the shared endpoint implementation is now
introduced with its contract in 1b and tested with its consumers. The former
`coll-work-8-expanded-tests` aliases `coll-work-8`: expanded regressions now live
with their owning features. These aliases have no separate review diff.
`coll-work-fixes` points to the integrated contention tip.

Old tips are retained under `backup/coll-stack-pre-fold-20260907/` in both repos.
The first pre-fold validated production tree remains at
`backup/coll-fold-reference-20260907` in Elements and
`backup/coll-design-validated-20260907` in Core. The subsequent merge-readiness
fold adds job-scoped Mercury service configuration, host VN/VC credit
normalization, transactional rejection of remapped service VNs, and the LRU
support policy. Its old tips and exact working-tree snapshot are retained
under `backup/coll-merge-readiness-20260907/`. The subsequent scope cleanup retains
all previous tips under `backup/coll-scope-cleanup-20260907/` in both repos.

## Validation and scope cleanup

The cleanup removes unused service enumeration and participant forwarding,
accepted-operation recovery scaffolding, the obsolete processor egress-capacity
parameter, and active-service RR. Native tests share rank-separated CSV parsing;
the scalar processor's duplicate test fixture is consolidated without dropping
its unique assertions. The router output-queue depth remains a modeled limit.

Final cleanup validation, exact paired commits, per-layer counts, and follow-up
branch details are in the workspace sibling directory
`cleanup-validation-20260907`. Those results describe the earlier published cleanup. Later Core refinements
and their validation are recorded in `review-fixes-20260908`; previous artifacts
remain available for comparison.

Earlier design and contention validation established:

- 18 Core serialization tests passed.
- 65 selected Elements tests passed; nine unrelated optional-dependency tests skipped.
- Seven selected two-thread tests passed.
- Layer-local syntax, Python, manifest and dependency checks passed.
- The independent-output RR starvation regression failed before the correction
  and passed afterward, including blocked, credit-starved, continuously ready
  synthetic outputs and mixed ordinary VCs.
- All 40 mixed-traffic simulations passed, six disabled/dormant ordinary-statistics
  pairs matched exactly, and every ordinary endpoint progressed in every complete
  sample during collective activity. Five metric-parser regressions passed.

The three configuration fixes subsequently passed 34 relevant regression
tests and three focused two-thread checks. Previous paired-commit validation,
including Linux and multi-process simulation, is recorded with exact commands,
environment details, commit IDs, and results in the workspace sibling directory
`merge-validation-20260907`. Hardware timing calibration remains open.

See [experiment instructions](src/sst/elements/merlin/tests/mixed_collective_experiments.md)
for commands, parameters, measurement windows and limits. Run results retain raw
stdout/stderr, statistics, motif windows, exact commands and binary hashes. The
7 September run is in the workspace sibling directory
`mixed-traffic-results-20260907`, with `findings.md`, `summary.md`, CSV and JSON.


## Core envelope scope

The Core review adds service metadata, capability discovery, Request ownership
operations, and envelope validation. Service data uses the existing polymorphic
`SST_SER` pointer path. The general serializer implementation is unchanged from
the Core review base.

Native payload copy and overwrite behavior is preserved. Requests own their
attached service data; give/take/clear and independent sidecar clones maintain
that contract. Standard pointer tracking can restore nonowning aliases, including
an alias serialized before its owning Request. It does not enforce ownership,
add family-aware construction, or provide exception rollback. Envelope validation
checks decoded service fields under SST's existing registered-type conventions.

The current scope and validation evidence are recorded in the workspace sibling
`core-envelope-scope-20260916`. Earlier reports remain historical evidence for
the versions they tested. Mercury checkpointing remains unsupported; bounded
chunks and floating-point compiler-policy changes remain outside this review.

## Prototype removal

The prototype-free stack and targeted validation are recorded in the workspace
sibling `prototype-removal-20260921`. Earlier tips are retained under
`backup/prototype-removal-20260921/`. Apart from restoring the ordinary MTU
default and updating this guide, layer 2 and later source trees are unchanged.

## Include convention

Layer 1b's Merlin `Makefile.am` uses the upstream `AM_CPPFLAGS +=` form and the
contract test includes its headers by relative path. Later layers carry the
same two hunks and are otherwise unchanged. Validation is recorded in the
workspace sibling `relative-includes-20260921`; earlier tips are retained under
`backup/relative-includes-20260921/`.
