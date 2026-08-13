# PR3 code review: `pr3-ficollective-bridge`

Review range: `upstream/devel..pr3-ficollective-bridge`  
Reviewed tip: `7929e7b48`  
Verdict: **Changes requested.**

## Findings

### [P1] Preserve immediate completions

File: `src/sst/elements/iris/libfabric/prov/sumi/src/sumi_coll.cc:151`

Every bridge operation discards the `CollectiveDoneMessage*` returned by the
engine and unconditionally calls `blockUntilNext()`. Single-rank collectives
complete immediately and never post another message, so `NRANKS=1` stalls at
the first barrier. The multi-rank path also leaks the message returned by
`blockUntilNext()`.

Capture the engine return, block only when it is null, then delete the done
message. Apply this to all operations.

### [P1] The advertised `FI_COLLECTIVE` lifecycle is incomplete

Files:

- `src/sst/elements/iris/libfabric/prov/sumi/include/sumi_prov.h:109`
- `src/sst/elements/iris/libfabric/prov/sumi/src/sumi_fabric.cc:195`

The provider advertises `FI_COLLECTIVE`, but does not install
`fi_query_collective`, AV-set operations, or `fi_join_collective`. A conforming
client therefore cannot discover supported operations, create/join a group, or
obtain a valid `coll_addr`. The test bypasses this by passing `FI_ADDR_UNSPEC`.

Implement the lifecycle and group-to-communicator mapping, or stop advertising
the capability until it exists. The test should request `FI_COLLECTIVE` in its
hints and exercise query/join rather than bypassing them.

### [P1] Decode and honor collective addresses

File: `src/sst/elements/iris/libfabric/prov/sumi/src/sumi_coll.cc:184`

All operations ignore `coll_addr` and run over the world communicator. Rooted
operations then cast `root_addr` directly to `int`, although SUMI encodes the
rank in the upper 32 bits of `fi_addr_t`. A valid nonzero root can consequently
become rank zero or garbage.

Resolve `coll_addr` to the joined communicator, decode `root_addr` through the
provider address representation, and validate that the root belongs to the
group. The cast appears in broadcast, reduce, gather, and scatter.

### [P1] Do not expose aborting reduce-scatter

File: `src/sst/elements/iris/libfabric/prov/sumi/src/sumi_coll.cc:292`

The operations table exposes `sumi_ep_reduce_scatter`, but the underlying
`HalvingReduceScatterActor::initDag()` aborts with
`halving_reduce_scatter: not implemented`.

Install `fi_coll_no_reduce_scatter` or return `-FI_EOPNOTSUPP` until a real
algorithm exists, and report it unsupported through `fi_query_collective`.

### [P1] Use an engine/group-wide, synchronized operation sequence

Files:

- `src/sst/elements/iris/libfabric/prov/sumi/include/sumi_prov.h:333`
- `src/sst/elements/iris/libfabric/prov/sumi/src/sumi_coll.cc:144`

`coll_tag` is stored per endpoint, while the `CollectiveEngine` is shared per
rank and indexes active collectives by `(type, tag)`. Two endpoints therefore
reuse tag zero for their first operation and can be merged into the same engine
collective. Additionally, `coll_tag++` is an unsynchronized data race despite
the provider advertising `FI_THREAD_SAFE`.

Sequence operations per joined group in engine/transport state. Synchronize
allocation and match completions by type/tag rather than accepting the next
message from the shared default CQ.

### [P2] Reject calls that cannot produce a completion

File: `src/sst/elements/iris/libfabric/prov/sumi/src/sumi_coll.cc:116`

`deliverCompletion()` silently returns when no transmit CQ is bound, but the
collective API still returns `FI_SUCCESS`. A client waiting for the promised
completion then blocks forever.

Validate endpoint/CQ state before starting the collective and return an
appropriate error when completion cannot be delivered.

### [P2] Honor operation flags and completion semantics

Files:

- `src/sst/elements/iris/libfabric/prov/sumi/src/sumi_coll.cc:116`
- `src/sst/elements/iris/libfabric/prov/sumi/src/sumi_coll.cc:184`

Broadcast ignores libfabric's required roles: the root should pass `FI_SEND`
and receivers `FI_RECV`, while the test passes zero. The synthetic completion
also reports `FI_SEND` for every collective on every rank.

Validate flags and rank roles, then generate completion flags appropriate to
the operation. Reject unsupported flag combinations.

### [P2] Validate `count` before narrowing

File: `src/sst/elements/iris/libfabric/prov/sumi/src/sumi_coll.cc:130`

The public API accepts `size_t count`, but every implementation narrows it to
`int` without checking. Counts above `INT_MAX` wrap or truncate before buffer
sizing.

Return `-FI_EMSGSIZE` for unsupported counts or retain a sufficiently wide type
throughout the engine.

### [P2] Make the test runnable and register it

Files:

- `src/sst/elements/iris/libfabric/tests/test_fi_allreduce.py:31`
- `src/sst/elements/iris/libfabric/Makefile.am:25`

The documented `sst test_fi_allreduce.py` command fails because
`platform_file_mask_mpi_test` resides in another test directory. The test is
only listed in `EXTRA_DIST`, so `sst-test-elements` does not run it.

Make the driver self-contained or provide its import path, add an SST testsuite
entry, and ensure the harness fails when no application prints `PASS`. At
present the single-rank deadlock reaches an empty event queue while SST exits
with status zero.

### [P2] Test the actual registry parameter

Files:

- `src/sst/elements/iris/libfabric/tests/test_fi_allreduce.py:35`
- `src/sst/elements/iris/libfabric/prov/sumi/src/sumi_coll.cc:24`

The test and bridge comment advertise `app1.allreduce_alg`, but PR2's supported
key is `app1.collective.allreduce`. The bogus parameter is silently dropped.
Tests still appear to select the requested algorithm because
`SUMI_ALLREDUCE_ALG` remains in the environment and activates the fallback.

Use `app1.collective.allreduce` and test the parameter and environment paths
independently.

### [P3] Correct the capability comment in the client

File: `src/sst/elements/iris/libfabric/tests/fi_allreduce_test.cc:54`

The comment says the provider does not advertise `FI_COLLECTIVE`, but this
commit adds it to both capability masks. Update the comment and use collective
hints so the test verifies capability negotiation.

## Verification

- `git diff --check upstream/devel...HEAD`: passed.
- Built libfabric, its raw client, SUMI, and Mercury successfully.
- The documented test command failed with `ModuleNotFoundError`.
- After manually adding `mask-mpi/tests` to `PYTHONPATH`:
  - Default 8-rank test passed.
  - Ring and recursive-doubling tests passed at 2, 3, 4, 5, and 8 ranks.
  - The 1-rank run printed no `PASS`, reached an empty event queue, and SST
    nevertheless returned exit status zero.

## Cross-review note

This review incorporates validated additional findings from
`claude-pr3-review.md`. Its reported `FI_MIN` defect is not present in the
reviewed tree: `Min::op` contains only the correct minimum assignment. Its SEP
NULL-collective-ops claim is also not included because the SEP implementation
is currently compiled out under `#if 0`; that pre-existing stub is outside this
PR's operative path.

## Reference

The lifecycle, group-address, root-address, flag, and completion findings follow
the [libfabric collective API contract](https://ofiwg.github.io/libfabric/v1.9.0/man/fi_collective.3.html).
