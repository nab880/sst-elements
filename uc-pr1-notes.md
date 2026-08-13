# uc-pr1 notes: collective registry (+ local additions for the PR2 restack)

Branch: `uc-pr1-collective-registry`, based on `upstream/devel` (which now
includes the merged pr1-nic-flowkey, PR #2697). Local copy rebased
2026-07-09; **diverged from the pushed origin copy** — needs a force-push.

Planned merge order: devel -> **uc-pr1** -> uc-pr2 (FI_COLLECTIVE bridge)
-> pr2 (ring reduce-scatter) -> pr3 (GPU library + staging).

## What the branch is (first three commits, as pushed)

1. `iris/sumi: user-selectable collective algorithm registry + ring allreduce`
   — name-keyed `CollectiveRegistry` ((op, name) -> factory),
   `SUMI_REGISTER_COLLECTIVE` macro, param > env > default selection
   (`app1.<op>_alg` / `SUMI_<OP>_ALG`), built-ins registered, the ring
   all-reduce port, pymercury whitelisting, mask-mpi allreduce +
   my_collective prototyping tests + collective_sweep.sh.
2. `registry startAlgOverride helper + collective fixups` — one shared
   override path; ring finalizeBuffers long-widening; reduce.h toString fix.
3. `review fixes` — ext-lib LDFLAGS, ring `>` round-guard off-by-one, single
   alg_keys table, pymercury cross-referencing, sweep env hygiene.

## Fourth commit added locally 2026-07-09 (for the restack)

`iris/sumi: raise max_round for the ring, drop stub reduce-scatter
registration, add in-suite ring test`:

- **`Action::max_round` 500 -> 4096** (moved here from PR2): uc-pr1 ships and
  registers the ring all-reduce, whose `2*(N-1)` rounds hit the old budget at
  252 ranks — the budget raise belongs with the algorithm that needs it.
  Inherited nit: the bump shrinks the safe partner-rank ceiling in the
  uint32 message-ID encoding from ~1.4M to ~174K ranks (round-guarded, not
  rank-guarded; fine at realistic scales).
- **Dropped the `"halving"` reduce_scatter registration** from
  `collective_registry_builtins.cc`: at this point in history the class is
  still an abort stub, so the name advertised a selectable crash — and PR2
  turns the class into a *ring*, which would have forced a user-visible
  rename. PR2 now adds the registration as `"ring"` when it implements it.
- **Removed the dead `result_buffer_ = dst;` no-op** in
  `ring_allreduce.cc::initBuffers()` (same cleanup PR2's copy got before the
  ring moved here).
- **`test_ring_allreduce` moved here from PR2** (8 ranks, nelems=19 — not
  divisible by the rank count, covering remainder chunks; `MPI_Allreduce` +
  `MPI_Iallreduce`; every rank checks the full result). Selection switched
  from the `os.environ` hack to the clean `app1.allreduce_alg = "ring"`
  param this branch provides. Without this, the ring would land upstream
  with no CI exercising it (the pre-existing `test_allreduce` runs the Wilke
  default; the sweep script is out-of-suite). Verified the param actually
  takes: 26.26us (ring) vs 12.30us (default) for the same app, identical
  numeric results.

## Verification

Suite: `sst-test-elements -w "*mask_mpi*"` green at the stack tip, including
`test_allreduce` (default alg, byte-identical to its refFile),
`test_my_collective`, and the migrated `test_ring_allreduce` (refFile
regenerated at the new base — timing identical to the pre-migration run,
26.2638us).
