# Carcosa + VLA: ECC Cost-Benefit Study Methodology

> **Branch scope: `ecc_all_p1`.** This branch focuses the case-study
> driver on **Phase 1 only** (real `vla_cpu` + `vla_gpu` RISC-V binaries
> through `testCarcosaVLA_GPUCPU.py`). The headline matrix is 5 cases
> (C0..C4) x 3 schemes (`none`, `secded`, `chipkill`) x 1 seed = 15 runs
> on the manifest default latency profile, 8 pipeline cycles each. The
> Phase-2 synthetic-delay-agent sweeps (`run_ecc_sweep.sh`,
> `pilot_case_goldilocks.sh`, the fast/strict Pareto slice) are still
> present in the repo but are not invoked by the case-study or preflight
> drivers on this branch; see git history (`ecc-vla`) for the Phase-2
> driver.
>
> Phase 1 also runs a **propagation analyzer** that joins ECC events
> with per-frame ActionScorer outcomes to produce
> `propagation_trace.csv` + `propagation_summary.csv` under
> `case_studies/analysis/`, so a single CSV row tells the
> "fault landed here -> ended up over there" story without log
> grepping.

This document describes the SST-Elements / Carcosa pipeline used to study
ECC schemes and placement policies on a representative workload. The
contribution of this artifact is the *framework* and the *metric set*,
not a deployment-calibrated BER or FIT threshold. It exists to make the
results in
`run_ecc_sweep.sh` -> `analyze_ecc_results.py` -> `make_figures.py`
(plus `run_case_studies.sh` -> `make_figures.py --case-studies`)
reproducible. See the paper for the full framing; this README is the
operational companion.

## 1. What the pipeline measures

For each combination of `(scheme, kernel/region policy, BER (or FIT),
fault_model, due_action, payload_dtype, seed)` we measure benefit and
cost on the same metric axes (paper Sec. "Cost-Benefit Metrics"):

| Quantity                | Source                                           | Used by                              |
| ----------------------- | ------------------------------------------------ | ------------------------------------ |
| `events_clean/correctable/due/escape` | EccGuard per-kernel block          | `fig_ber_sensitivity`, `fig_fault_mode_mix` |
| Per-region split        | EccGuard per-kernel-per-region block             | `fig_attr`                           |
| Fault-mode mix          | EccGuard fault-mode draws block                  | `fig_fault_mode_mix`                 |
| `escape_high_blast/low_blast`, `frames_aborted` | EccGuard escape/abort summary | `fig_due`                       |
| End-to-end pipeline ps  | VLA per-kernel profile                           | calibration, `fig_pareto` (cost axis) |
| Frame drops             | VLA frame-drops summary                          | `fig_due`, Table 1                   |
| `unsafe_action_rate` / `argmax_change_rate` | Action Scorer per-frame summary | `fig_pareto`, `fig_ber_sensitivity`, Table 1 |
| Outcome classes O1..O4  | CriticalActionWatcher + Action Scorer (campaigns)| `fig_sanity`                         |

These map to the paper's experimental narrative:

- **`fig_sanity`** (Case Study A): on deterministic single-bit, double-bit,
  and multi-chip campaigns C0-C4, does each ECC scheme produce the
  expected outcome distribution? This is the framework's behavioural
  sanity check.
- **`fig_pareto`** (Case Study A): cost-benefit Pareto frontier from
  campaign C2 with three latency profiles per scheme. Headline cost vs
  benefit plot.
- **`fig_policy`** (Case Study B): policy granularity at fixed scheme
  (`uniform` vs `kernel_aware` vs `region_aware` vs `full`) - currently
  a placeholder; renderer pending.
- **`fig_attr`** (Case Study B): workload-visible escape attribution
  by `(kernel, region)`. Localises remaining escapes so a finer-grained
  policy entry can target them.
- **`fig_due`** (Case Study B): DUE-response trade-off
  (`drop_frame` vs `latency_only`).
- **`fig_ber_sensitivity`** (Case Study B, sensitivity): unsafe-action
  rate vs BER per `(scheme, policy)`. BER is treated as a controlled
  stress dimension, not a deployment threshold.
- **`fig_fault_mode_mix`** (supplementary): JEDEC fault-mode mix vs
  BER, reported for transparency on what the sampler is producing.
- **Table 1**: Headline metrics per `(scheme, policy)` on the canonical
  slice with Wilson 95% CIs.

## 2. Two-phase simulation

### Phase 1: real RISC-V binaries

```
$ cd VLA-Example/tests
$ sst testCarcosaVLA_GPUCPU.py 2>&1 | tee phase1.log
$ eval "$(python3 ../scripts/extract_baselines.py --emit-env phase1.log)"
```

This emits `VLA_BASELINE_CPU_PS` and `VLA_BASELINE_GPU_PS` (one per VLA
kernel). Phase 1 also doubles as a *spot check* for ECC latency under real
binaries — see `run_ecc_phase1_spotcheck.sh`.

### Phase 2: synthetic delay agents (the sweep)

```
$ cd VLA-Example/tests
$ ../scripts/run_ecc_sweep.sh                       # runs the matrix
$ python3 ../scripts/analyze_ecc_results.py ./ecc_sweep_out
$ python3 ../scripts/make_figures.py ./ecc_sweep_out/analysis
```

The sweep iterates over a configurable set of axes; `run_all_ecc.sh`
exposes three named profiles. BER and FIT are treated as controlled
stress dimensions (sensitivity analysis), not deployment thresholds:

| Profile | When to use | Axes |
|---------|-------------|------|
| `FAST=1` | Smoke test the pipeline. ~1 minute. | `BERS=1e-4`, `SEEDS=1`, `FAULT_MODELS=poisson`, `VLA_MAX_CYCLES=1`. |
| `HEADLINE=1` | Main-text Case Study B figures (`fig_attr`, `fig_due`, `fig_ber_sensitivity`) and Table 1. | `BERS="0 1e-7 1e-5 1e-4"`, `SCHEMES="none secded chipkill"`, `POLICIES="uniform kernel_aware region_aware"`, `FAULT_MODELS="jedec_mix"`, `DUE_ACTIONS="drop_frame"`, `SEEDS="1 2 3"`, `VLA_MAX_CYCLES=8`. ~84 cells (21 BER=0 goldens + 63 BER>0). |
| `FULL_CUBE=1` | Full design-space cube; required for both `due_actions` to feed `fig_due`. ~1k+ Phase-2 cells, hours of wall-clock. | `BERS="0 1e-12 ... 1e-6"`, `SCHEMES="none secded chipkill"`, `POLICIES="uniform kernel_aware region_aware full"`, `FAULT_MODELS="poisson jedec_mix"`, `DUE_ACTIONS="latency_only drop_frame"`, `SEEDS="1..5"`. |

Case Study A (sanity campaigns + Pareto plot) is produced by a
separate driver, `run_case_studies.sh`, which fires deterministic
fault campaigns (C0-C4) with `EccGuard`'s
`campaign_target_kernel=ACTUATE` /
`addr_filter_region=action_queue` /
`campaign_max_events_per_kernel_entry=1` parameters and feeds
`make_figures.py --case-studies` to produce `fig_sanity` and
`fig_pareto`.

Any individual axis exported in the environment overrides the profile
default; e.g. `HEADLINE=1 SEEDS="1 2 3 4 5"` keeps the headline slice
but pools 5 seeds for tighter Wilson CIs. `PAYLOAD_DTYPE` (`bytes`,
`bf16`, `fp8`, `int8`) is orthogonal to the profile and defaults to
`bytes` everywhere.

Region routing is driven by the `VLA_REGIONS` environment variable; the
default partitions DRAM into `weights`, `kv_cache`, `activations`, and
`action_queue`, and the `KERNEL_POLICY_AWARE` / `REGION_POLICY_BASE` /
`REGION_POLICY_FULL` CSVs select different policies for each.

## 3. Calibration

`fault_model='jedec_mix'` samples one of {single-cell, single-word,
single-row, single-column, single-bank, single-device} fault events per
access. Default mixture weights are taken from

> Sridharan, Stearley, DeBardeleben, Blanchard, Gurumurthi.
> *"Memory Errors in Modern Systems: The Good, The Bad, and The Ugly,"*
> ASPLOS 2015 (Table 4),

cross-checked against

> Schroeder, Pinheiro, Weber. *"DRAM Errors in the Wild: A Large-Scale
> Field Study,"* SIGMETRICS 2009.

Bit-error spans per mode (`kFaultModeBitsLow / kFaultModeBitsHigh` in
`Components/EccGuard.cc`) are intentionally conservative for SECDED-64 and
ChipKill x4 classification. They can be overridden via the
`fault_mode_weights` parameter for sensitivity sweeps.

`fit_per_mbit_per_hour` lets a reviewer convert the BER knob into a single
calibrated FIT number; the conversion is logged in `setup()` for
transparency.

## 4. Action Scorer (behavioral metric)

`Carcosa.ActionScorer` reads `PipelineStateBase::frames` (populated by the
VLA pipeline agents on every completed pipeline cycle) and computes:

- `dropped` (frame was aborted via `due_action='drop_frame'`)
- `escapes_in_frame` / `flips_in_frame` (counter deltas pulled from the
  registry's cumulative ECC counters)
- `argmax_changed` (workload-defined `actionChecksum` differs from the
  golden trajectory)
- `safety_violated` = `dropped OR argmax_changed`

The golden trajectory is loaded from a CSV file via the `golden_log`
parameter (format: `pipeline_cycle,kernel_at_close,action_checksum`); set
`emit_golden=true` on a fault-free baseline run to dump a fresh golden log
under the `=== Action Scorer ... Golden Emit ===` block. For the headline
runs, the workload should hash its actual actuator output into
`actionChecksum`; the synthetic delay agents in this repository provide a
simple FSM-derived checksum so the scoring scaffolding is end-to-end
without requiring a real VLA binary.

### 4.1 Golden file keying and `golden_required`

`run_ecc_sweep.sh` keys golden files by **`(scheme, policy)`** only --
`goldens/golden_<scheme>_<policy>.csv` -- and the **first successful
BER=0 run for that pair wins**: subsequent BER=0 cells with different
seeds reuse the file already on disk. This is intentional and safe as
long as the FSM trajectory is deterministic across seeds, which is the
case when `VLA_DECODE_EXIT_PROB=0.0` (the default). Set
`ACTION_SCORER_FORCE_REGEN_GOLDEN=1` to overwrite the file.

If you raise `VLA_DECODE_EXIT_PROB > 0`, the FSM kernel sequence becomes
seed-dependent and the cross-seed golden reuse silently produces
spurious `argmax_changed=1` flags. In that regime you must either pin
`SEEDS=1` or extend `golden_path_for` / `golden_target` in
`run_ecc_sweep.sh` to include the seed.

The Action Scorer fatals at `setup()` if `golden_log` is set but the
file cannot be opened or contains no parsed entries. Previously the
scorer fell through silently and reported `unsafe_action_rate = 0` for
every frame regardless of BER, which produced publication-grade CSVs
that quietly under-reported safety violations. To opt back in to the
legacy behavior (e.g., for self-replay sanity checks), set
`ACTION_SCORER_GOLDEN_REQUIRED=0` on the SST run; the scorer logs a
WARNING and reports `argmax_changed=0` for every frame.

The analyzer (`analyze_ecc_results.py`) refuses to ingest any run whose
ActionScorer Summary block is missing -- the run is moved to
`rejected_runs.csv` with `reason=missing_scorer_summary` instead of
silently mixing scorer counts with the VLA agent's cumulative
`frames_dropped` block. `pressure_points.csv` and `per_frame_safety.csv`
therefore agree row-for-row on `frames_dropped`, `frames_unsafe`, and
`safety_violated` for every cell.

## 5. Reproducing each figure

```
# Case Study B figures from the sweep CSVs
$ ../scripts/make_figures.py ./ecc_sweep_out/analysis

# Optional: change the unsafe-action budget reference line on
# fig_ber_sensitivity (must match the analyzer's --unsafe-budget so
# the preflight crossing check uses the same threshold)
$ ../scripts/make_figures.py ./ecc_sweep_out/analysis --unsafe-budget 1e-7

# Case Study A figures (sanity campaigns + Pareto plot from C2)
$ ../scripts/run_case_studies.sh
$ ../scripts/analyze_ecc_results.py ./case_studies_out --case-studies
$ ../scripts/make_figures.py ./case_studies_out/analysis --case-studies
```

When `make_figures.py` is invoked without `--case-studies` and the
case-study artifact has not yet been produced, the renderer emits
blank placeholder PDFs for `fig_sanity.pdf` and `fig_pareto.pdf` so
the LaTeX paper still compiles. The `fig_policy.pdf` is currently a
permanent placeholder until the policy-comparison renderer is wired
up; the underlying data is already in `pressure_points.csv`.

Output paths:

```
ecc_sweep_out/
  analysis/
    per_run_summary.csv
    per_kernel_overhead.csv
    per_region_overhead.csv
    per_frame_safety.csv
    fault_mode_mix.csv
    pressure_points.csv
    pressure_points_full.csv         # full factorial; feeds fig_due
    summary.txt
    table1_headline.csv
    figs/
      # Case Study A (from `make_figures.py --case-studies`):
      fig_sanity.pdf                 # outcome distribution C0-C4 x scheme
      fig_pareto.pdf                 # cost-benefit Pareto from C2

      # Case Study B (from `make_figures.py`):
      fig_policy.pdf                 # placeholder (renderer pending)
      fig_attr.pdf                   # (kernel, region) attribution
      fig_due.pdf                    # drop_frame vs latency_only
      fig_ber_sensitivity.pdf        # unsafe-action vs BER sensitivity
      fig_fault_mode_mix.pdf         # supplementary: JEDEC mix vs BER

      # Supplementary (from `make_figures.py`):
      fig_attr_kernel.pdf            # kernel-only attribution bars
      fig6_campaign_attribution.pdf  # campaign-mode bars (when present)
```

## 6. Known modelling limitations

### 6.1 Checksum timing window (GAP-1)

The stub publishes its `actionChecksum` via MMIO 0x30 immediately before
the status write that triggers FSM advance. Escapes on cache writebacks
or coherence traffic that arrive *after* the checksum write but before
the analytical delay completes cannot affect `argmax_changed` for that
frame. This window is small and predominantly contains protocol traffic,
not user-data loads; the bias on `unsafe_action_rate` is negligible at
BER <= 1e-4.

### 6.2 Synthetic DRAM regions (GAP-3)

Phase-2 region-aware policies use synthetic DRAM region addresses
configured via `REGIONS_CSV` (default: `weights:0x10000000:0x4000000`,
...). The `VLACpuDelayAgent` stub re-publishes real BSS virtual
addresses at runtime via the MMIO region-commit ABI, but the stub's
buffers are small (64 KiB weights, 4 KiB action_queue) --
representative of memory-access cadence, not full tensor footprints.
Region-aware policy results therefore demonstrate that the policy
*switching logic* is correct, not that the traffic distribution matches
a production VLA deployment.

### 6.3 Chipkill classification (GAP-2, now per-chip)

The current build uses `classifyEccWordChipAware` for `CHIPKILL_x4`:
each fault draw records which x4 chips are affected, and the classifier
grants Correctable only when all errors are within a single chip, DUE
for <= 2 affected chips, and Escape otherwise. The legacy per-bit
threshold (`<= 4` Correctable, `<= 8` DUE) is retained as a fallback
when chip data is unavailable. At the HEADLINE BER grid (<= 1e-4) the
two agree; divergence grows above BER ~ 1e-3.

### 6.4 Writeback vAddr=0 attribution (GAP-4)

Cache writebacks/evictions generated by the coherence protocol may carry
`vAddr=0`; `resolveRegionIdForEvent` falls back to the physical address,
which may not match any published virtual region. These events are
counted in the "unlabeled" region bucket. A `stat_vaddr_zero_fallback`
counter tracks how many events took this path so the preflight report
can flag the fraction.

## 7. Threats to validity (future work)

The framework deliberately stays out of the following tiers. Each is
also a natural extension on the framework's extensibility surface
(scheme, policy, fault model, DUE response, workload):

- **Cache/SRAM ECC**: would need a new SST element wrapping
  `memHierarchy.Cache` to install per-line ECC.
- **Lockstep DMR**: a parallel guard component cross-checking redundant
  copies of each access; adds a new "consensus" benefit metric.
- **Real OpenVLA / Pi0 / RT-2 dimensions**: rebuild `vla_cpu.c` /
  `vla_gpu.c` with realistic layer counts and tile sizes; absolute
  latencies in the framework's outputs become more meaningful, but the
  cross-design-point ordering is what is claimed today regardless.
- **On-die + rank-level layered ECC stack**: cascade two `EccGuard`
  instances with different schemes.
- **CXL poison forwarding**: add a new memEvent flag and have EccGuard
  propagate "poisoned" reads to the consumer; becomes a third value
  of the `due_action` axis.
- **Deployment-grade FIT calibration**: pin
  `fit_per_mbit_per_hour` against validated field FIT data for a
  specific DRAM technology. The framework supports it; the
  calibration itself is downstream work.

These are deliberately out of scope so the paper's threats-to-validity
section has a clear list to point at.
