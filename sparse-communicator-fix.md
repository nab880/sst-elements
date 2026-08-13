# Sparse communicator multi-rank fix plan

## Finding

The time fault is a downstream symptom of malformed communicators, not an
all-reduce hierarchy bug.

`MpiCommFactory::commSplit()` has two metadata-exchange paths. The active path
stores `{next_id, color, key}` in the process-local `comm_split_entries` map and
runs an allgather with null buffers only to model its timing. That works when all
simulated ranks share one process, but not when SST runs on multiple processes.
Each SST process reads uninitialized entries for simulated ranks owned by the
other process and constructs a different communicator membership.
Collectives then use incompatible domain ranks and tags, leaving unmatched
events until the SST synchronization fault appears.

The path is selected by `SST_HG_DISTRIBUTED_MEMORY`, which is not defined in
sst-elements and therefore evaluates to zero. This has been present since the
Mask-MPI import. A compile-time choice is also unsuitable because the same
element build can run with one or several SST ranks.

Instrumenting the 8-rank `rank % 4` split under two SST ranks showed sizes 1 and
5 instead of four size-2 communicators. Forcing the real allgather produced the
expected memberships `{0,4}`, `{1,5}`, `{2,6}`, and `{3,7}`; both flat and
hierarchical sparse all-reduce then completed without the SST time fault.

## Implementation

1. In `mpi_comm_factory.cc`, always exchange the three split-control integers
   through the existing blocking Mask-MPI allgather on the parent communicator.
   Use owned storage, for example:

   ```cpp
   std::array<int, 3> mydata{next_id_, my_color, my_key};
   std::vector<int> result(3 * caller->size());
   parent_->allgather(mydata.data(), 3, MPI_INT,
                      result.data(), 3, MPI_INT, caller->id());
   ```

2. Keep the existing communicator-ID maximum agreement, color filtering, key
   ordering, and task mapping unchanged.

3. Delete the process-local `comm_split_entries` path, mmap variant, refcount
   cleanup, obsolete preprocessor guards, and their unused includes and lock
   declarations. This also removes a leak in the dormant real-allgather path,
   whose `new[]` result is never freed, and a dangling map entry left after the
   static-path buffer is freed.

4. Do not add a runtime single-process shortcut. The control payload is only 12
   bytes per simulated rank, and one correct path avoids divergent behavior.

## Regression coverage

Prefer extending the existing sparse path rather than adding another test-suite
case:

- Keep 8 simulated ranks split by `rank % 4`, ensuring members cross SST
  process boundaries under `-r 2`.
- Validate `MPI_Comm_size == 2`, the communicator rank, and the gathered world
  ranks for every color before running a collective.
- Retain blocking and nonblocking all-reduce validation and `MPI_Comm_free`.
- Repeat split/free within the application to catch collective-tag or context-ID
  drift after removing the extra manual tag consumed by the static path.
- Include a nested split in the same application to exercise a non-world parent
  communicator without adding another test-suite case.
- Add key-order validation, including equal keys; cover `MPI_UNDEFINED` in the
  same application if supported cleanly.
- Remove the multi-rank skip from
  `test_hierarchical_allreduce_sparse_comm_warning` once the regression passes.
- Keep exact output comparison. Update reference timing only if the real control
  payload changes deterministic simulated time.

## Verification

1. Build `libmask_mpi`, `libsumi`, and Mercury.
2. Run the sparse communicator test with one and two SST ranks; also run `-r 4`
   manually if available.
3. Run the sparse application in both flat and hierarchical all-reduce modes to
   prove the fix is below topology policy.
4. Run the full Mask-MPI suite with one and two SST ranks.
5. Run `git diff --check`.

## Separate follow-ups

These existing lifecycle issues did not cause this fault and should not be
folded into the focused fix without separate tests:

- `MPI_Comm_free` deletes a communicator even if it has outstanding requests.
- `Communicator` does not delete its allocated SMP and owner communicators.
- Split communicator groups are assigned IDs but are not registered in
  `grp_map_`.
- Dup/create/cart communicators do not initialize SMP topology as split
  communicators do.
- Mixed node occupancy can make only some ranks enter SMP topology discovery;
  that needs a separate collective topology-design fix.
- Communicator IDs are truncated when encoded into collective tags.
- The large-payload multi-rank `NetworkMessage` invalid free is unrelated.
