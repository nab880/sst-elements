# PR3 notes: mercury/gpu (simulated GPU library + sst_hg_cuda ABI bridge)

Branch: `pr3-gpu-library`, based on `pr2-sumi-maskmpi` (based on `pr1-nic-flowkey`,
based on `upstream/devel`).
19 files, +1036/-17, two commits.

**Restacked 2026-07-09:** now carries a second commit, the GPU-aware MPI staging
formerly in PR2 (6 mask-mpi files, +81) — it depends on `hg_cuda.h` and
`App::hasLibrary()` from this PR's GPU-library commit, so it couldn't live in
PR2 without breaking PR2's standalone build. See item 7 below.

**Review fixes folded in 2026-07-09 (post-restack review):**
- **Staging chokepoint refactor** (staging commit): the PCIe charge moved from
  7 per-entry-point hooks into `MpiQueue::send` (D2H, covers blocking/Isend/
  persistent Start — persistent sends previously paid nothing) and
  `MpiQueue::recv` (arms a deferred H2D charge on the request, paid once at
  completion and sized by the *actual* received bytes from the status, not the
  posted count). Collectives now stage too: D2H in `startMpiCollective`, H2D
  armed in `addImmediateCollective` and paid at waitCollective/wait/test.
  `CollectiveOpBase::sendElems/recvElems` (virtual, CollectivevOp sums its
  per-rank counts) size the charges; op buffer/count fields got in-class
  defaults (barrier ops never ran startMpiCollective and carried garbage).
  The GpuLibrary probe is latched on first use, not in the constructor, so
  'libraries' list order can't silently disable staging.
- **Device-cookie classification** (gpu-library commit): GPU cookies looked
  like real pointers to `isNonNullBuffer`, so any payload-copying path
  dereferenced them — device-buffer collectives segfaulted in `putOnWire`
  (verified with a throwaway device-buffer allreduce: crash before, runs
  after), and pt2pt eager sends only survived by a platform accident of what
  happened to be mapped at low cookie offsets. Both `null_buffer.h` copies
  (mercury + iris) now classify `[0x4000000000000000, +1TB)` as
  not-real-data (`isGpuCookie`); `GpuLibrary::malloc` aborts past the 1TB
  cookie ceiling instead of escaping the range.
- **Verified**: throwaway 2-rank device-buffer allreduce sweep —
  gpu_direct=off 5.354ms vs on 2.721ms; the 2.63ms delta matches
  5×(2×4MB)/16GB/s + latencies analytically. run_test_cuda_mpi_halo PASS with
  times byte-identical to pre-refactor (907.259/882.139us). vecadd + streams
  lit tests PASS. Non-GPU mask-mpi tests (sendrecv/allgather/reduce/alltoall)
  byte-identical to refFiles.

## Main contributions

1. **`GpuComputeAPI`** (`gpu_api.h`, new). A small abstract interface --
   malloc/free/isDevicePtr/memcpy/launch/stream+event lifecycle/deviceSync --
   that decouples the ABI bridge from the concrete cost model.

2. **`GpuLibrary`** (`gpu_library.cc/h`, new, ~430+148 lines). The actual
   model, registered as an SST `ELI` library (`gpulibrary:GpuLibrary`) so an
   app opts in by declaring it. Per-stream busy-until timelines: the default
   (NULL) stream keeps legacy CUDA ordering (serializes with every other
   stream), explicit streams advance independently, so async launches/copies
   correctly overlap host compute and each other. Kernel time is roofline
   (`flops/peak` vs `bytes/bandwidth`, whichever dominates) with:
   - a **wave-quantization penalty** (`ceil(waves)/waves`, WS1-1b) when
     `gpu_sm_count` is set, modeling occupancy fragmentation;
   - a **dual roofline** (WS1-1a): kernels named in `gpu_tensor_kernels` charge
     against `gpu_tensor_peak_flops` instead of the scalar peak;
   - an optional **calibration table** (`gpu_kernel_times`, JSON) that
     exact-matches or log-log-interpolates measured `(threads, seconds)`
     samples per mangled kernel name, overriding the roofline, falling back
     with a once-per-kernel warning on a miss.
   - Device pointers are a **bump-allocated cookie range** (`isDevicePtr` is a
     range check); `free()` is a no-op, so the memory-footprint report at
     teardown is the run's high-water mark, not a live/final figure -- a
     deliberate cost-modeling simplification, not a leak in the real sense.
   - Optional capacity enforcement: `gpu_mem_capacity` + `gpu_mem_fatal` can
     abort a rank that overflows modeled device memory instead of just
     reporting it.

3. **`cuda_runtime_api.cc`** (new). The `sst_hg_cuda_*` extern-"C" ABI bridge
   -- one-line forwarders from each C entry point to the per-thread
   `GpuLibrary` (`Thread::current()->getLibrary<GpuComputeAPI>("GpuLibrary")`),
   aborting if the app never declared the library. This is what the compiler's
   launch rewrite (sst-hgcc) calls into.

4. **`hg_cuda.h`** (new). A vendored copy of sst-hgcc's ABI header; the file
   comment states the contract explicitly ("keep declarations identical;
   comments may differ") and a `static_assert(SST_HG_CUDA_ABI_VERSION == 1)`
   in `gpu_library.cc` is meant to catch drift. **Checked**: diffed byte-for-
   byte against `sst-hgcc/hgcc_include/libraries/hg_cuda.h` -- only comments
   differ, every signature and the ABI version (1) match exactly. No drift
   today.

5. **`App::hasLibrary()`** (`operating_system/process/app.h`, +4 lines). A
   non-aborting existence check, added so mask-mpi's device-pointer probe
   (item 7 below) can skip entirely for apps that never declared a GPU
   library, instead of calling `getLibrary()` (which aborts).

6. **`pymercury.py`** (+14 lines). Subscribes all 14 new `gpu_*`/`pcie_*`
   platform params so they flow from the platform definition into `HgOS`.
   **Checked**: every param `gpu_library.cc`'s constructor reads via
   `params.find<...>` has a matching entry here -- no silently-dropped params.

7. **GPU-aware MPI staging** (second commit; `mask-mpi/mpi_api.cc/h`,
   `mpi_request.h`, `mpi_api_send_recv.cc`, `mpi_api_test.cc`,
   `mpi_api_wait.cc`; moved here from PR2 in the 2026-07-09 restack). When
   `gpu_direct` is off, a device-pointer send/recv pays a PCIe staging cost
   (`pcie_latency` + bytes/`pcie_bandwidth`). Non-blocking receives defer the
   charge until the app actually observes completion (`armRecvStaging` /
   `stageRecvOnComplete`), mirroring what the blocking `doRecv` path already
   did inline. Verified this fires exactly once across every wait/test entry
   point (`wait`, `waitall`, `waitany`, `waitsome`, `test`, `testall`,
   `testany`, `testsome`) -- the `*any`/`*some`/`*all` variants all delegate
   through `test()`/`finalizeWaitRequest()`, no gaps.

## Nothing blocking found

Read through all 8 files plus the two cross-checks above (ABI header vs.
sst-hgcc's copy, and param list vs. what the constructor actually reads).
Didn't find anything analogous to PR2's rotation bug -- the stream/event
timeline logic, the cookie allocator, and the calibration lookup all look
correct and match their own doc comments.

## Worth knowing, not blocking

- `free()` being a no-op (see above) means long-running or memory-churning
  apps will over-report footprint relative to a real allocator. Fine for the
  demos this was built for (bounded allocation, checked once at teardown);
  would matter if this library is ever used for a workload that allocates and
  frees repeatedly and cares about a live footprint number rather than a
  high-water mark.
- `eventElapsedMs` returns `0.0f` if `stop`'s cursor isn't after `start`'s,
  rather than propagating a negative value or asserting. Defensive, and
  shouldn't trigger for well-formed event pairs, but silent zero is a
  slightly unusual choice if it ever does (vs. an abort that would surface a
  misuse).
