# PR2 notes: iris/sumi + mask-mpi (real ring reduce-scatter, async request plumbing)

Branch: `pr2-sumi-maskmpi`, based on `uc-pr2-ficollective-bridge` (based on
`uc-pr1-collective-registry`, based on `upstream/devel` — which now includes
the merged pr1-nic-flowkey, PR #2697).
9 files, +343/-36, three commits.

**Restacked 2026-07-09 (second time) onto the user-collectives PRs.** New
planned merge order: devel -> uc-pr1 (collective registry) -> uc-pr2
(FI_COLLECTIVE bridge) -> pr2 -> pr3. Consequences for PR2:
- **Ring all-reduce moved to uc-pr1** (which ships and registers it): the
  `ring_allreduce.cc/h` files, the `Action::max_round` 500->4096 bump, and
  the in-suite ring test (`test_ring_allreduce`, switched from the
  `os.environ` hack to the clean `app1.allreduce_alg` param uc-pr1 provides)
  all left PR2.
- **The `SUMI_ALLREDUCE_ALG` getenv hack no longer exists in any PR** — PR2
  never adds selection code; uc-pr1's registry (param > env > default) is
  already below it.
- PR2's reduce-scatter now registers itself as `"ring"` in
  `collective_registry_builtins.cc` (uc-pr1 deliberately does not register
  the class while it is still an abort stub).
- The round-budget guard got the same `>` off-by-one fix uc-pr1's ring
  received in review (abort only when `N-1 > max_round`; the largest round
  *index* is N-2).

Earlier restack (same day): the GPU-aware MPI staging moved to PR3 — it
depends on `hg_cuda.h`/`App::hasLibrary()` from PR3's GPU library. Backup
tags: `backup/pr2-pre-restack`, `backup/pr3-pre-restack`.

## Main contributions

1. **Real ring reduce-scatter** (`iris/sumi/reduce_scatter.cc/h`). Was a hard
   `abort("halving_reduce_scatter: not implemented")` stub; now a working ring
   implementation (the reduce phase of uc-pr1's ring all-reduce). This is what
   unblocks FSDP/ZeRO-3's per-layer reduce-scatter (previously impossible to
   simulate). Registered with uc-pr1's `CollectiveRegistry` as `"ring"` and
   also the engine default (the only implementation).

2. **Round-overflow guard**: the message-ID codec (`Action::messageId`) only
   round-trips below `Action::max_round`; the ring aborts with a clear message
   ("raise max_round for nproc=%d") when `N-1 > max_round` (largest round
   index is N-2) instead of silently corrupting message matching at scale.
   (The `max_round` 500 -> 4096 bump itself lives in uc-pr1 with the ring
   all-reduce.)

3. **Async collective request plumbing** (`mask-mpi/mpi_api_collectives.cc`).
   Wires `MPI_Reduce_scatter`/`MPI_Reduce_scatter_block` to the
   now-real engine (removing their `abort()` stubs) and adds non-blocking
   variants (`Ireduce_scatter`, `Ireduce_scatter_block`) so collectives can
   overlap backward compute -- the mechanism the CUDA demos' overlap knobs
   (`FSDP_PREFETCH`, `LLM_OVERLAP`, etc.) depend on.

4. **64-bit overflow guard** in `startReduceScatterBlock`: `count * nproc` is
   computed in `int64_t` first and aborts with a clear message if it would
   exceed `INT_MAX`, instead of silently wrapping negative and feeding garbage
   into the ring DAG.

5. **In-suite correctness test** (`mask-mpi/tests/test_reduce_scatter`,
   8 ranks): real host buffers through `MPI_Reduce_scatter_block` and
   `MPI_Ireduce_scatter_block`; every rank verifies it owns *its own* chunk of
   the elementwise sum -- the end-to-end regression lock for the rank-rotation
   fix below. (The ring all-reduce twin test moved to uc-pr1.)

## Fixed: reduce-scatter rank rotation + buffer-sizing (was a known limitation)

Originally found in review: `HalvingReduceScatterActor`'s ring reduce-scatter
had a documented off-by-one rank rotation (each rank ended up owning chunk
`(me+1) mod N`, not its own chunk `me`), harmless only because the sole
existing caller (CUDA demo cost modeling) always passes NULL buffers. A real
caller of `MPI_Reduce_scatter{,_block}` would have silently gotten the wrong
rank's chunk.

Investigating the fix surfaced a second, more fundamental issue: `initBuffers()`
aliased the caller's real `dst` (only `recvcnt`-sized, per the MPI contract) as
the algorithm's working buffer, which the DAG writes into at absolute offsets
spanning the *full* `nelems_` total -- an out-of-bounds write waiting for the
first real (non-NULL) caller, not just wrong data.

Both fixed in `reduce_scatter.cc/h`:
- **Rotation**: shifted `send_chunk`/`recv_chunk` by one round each (now
  `(me-r-1)`/`(me-r-2)` instead of `(me-r)`/`(me-r-1)`), so rank `me` now
  finishes owning chunk `me`. `ring_allreduce.cc` was deliberately left alone
  -- it has its own independent copy of this phase, and its all-gather half is
  wired to match the *original* rotation, so the two cancel correctly as-is.
- **Buffer sizing**: `initBuffers()` now allocates a fresh `nelems_`-sized
  scratch buffer for the ring math (matching the existing pattern in
  `reduce.cc` for its own real-vs-scratch buffer split) instead of aliasing
  `dst`. `finalizeBuffers()` now copies just this rank's chunk (`myChunkOffset_`
  / `myChunkCount_`, computed once in `initDag()`) out to the caller's real
  buffer (`realDst_`) before freeing the scratch. `memcopy()` is already a
  no-op on bytes when either side is a null-buffer sentinel, so NULL-buffer
  callers see no behavior change, just one more (correctly costed) copy.

**Verified locally** (not committed -- a throwaway harness, per instruction):
wrote a standalone C++ program mirroring the exact chunk-index arithmetic and
simulating the round-by-round ring across all N ranks in a single process (no
SST dependencies), checked against a brute-force elementwise-sum reference
across 11 cases (varying N, including remainder-distribution cases where
`nelems` doesn't divide evenly, and the N=1 edge case). All pass with the new
formula. Confirmed the harness is actually discriminating by running the same
cases through the *old* formula -- it fails every case, catching the original
bug.

## Minor, non-blocking nits

- ~~Dead no-op line in `RingAllreduceActor::initBuffers()`~~ — removed
  2026-07-09; the ring all-reduce (and this fix) now lives in uc-pr1.
- The `max_round` rank-ceiling nit (500->4096 shrinks the safe partner-rank
  ceiling in the message-ID encoding from ~1.4M to ~174K ranks) moved to
  uc-pr1 along with the bump itself. See uc-pr1-notes.md.
