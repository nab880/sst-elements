# GPU-driven FFT modeled in Quetz — demo runbook

A hand-written radix-2 Cooley–Tukey FFT executed on the **balar + GPGPU-Sim**
functional GPU model, driven two ways:

1. **Trace replay** (`QuetzTestCPU` / `BalarTestCPU`) — fast, deterministic, CI.
2. **Quetz QEMU sysmode guest** (`riscv_virt_balar_fft`) — a bare-metal RISC-V
   guest marshals balar wire packets and rings the GPU doorbell, so the run also
   models the **host-side cost of GPU offload** (scratch writes, flush-before-
   doorbell, MMIO, DMA) through the Quetz memHierarchy.

The headline is **architectural / performance modeling**. Correctness is a cheap
gate: a unit-impulse input has `X[k] = 1+0j` for every bin, which is *bit-exact*
in float32, so the existing exact-byte D2H check (`correct_memD2H_ratio == 1.0`)
doubles as a correctness assertion and a dropped-launch detector.

> Not cuFFT: GPGPU-Sim only executes PTX embedded in the binary; cuFFT's kernels
> are closed-source. The kernels here are our own (`balar_trace/fft.cu`).

## Files
| File | Role |
|---|---|
| `balar_trace/fft.cu` | `fft_bitrev` + `fft_stage` + `fft_scale` kernels and a single-block `fft_shared`; `main()` modes `staged`/`shared`/`roundtrip`; precision-generic (`-DFFT_DOUBLE`) |
| `balar_trace/fft_reference.py` | host twin: bit-exact impulse/DC + tone/round-trip validation; emits `.data`/`.trace`/firmware header; `--compare` (tolerance, `--roundtrip`), `--double` |
| `traces/fft_{impulse,dc}_256.trace`, `traces/fft_*_256.data`, `traces/fft_tw_128.data` | replay artifacts (committed for N=256) |
| `testBalar-fft.py`, `testQuetz-balar-fft.py` | trace-replay SDLs |
| `fft_stats_report.py` | tabulates SST stats for staged-vs-shared / N-sweep / trace-vs-sysmode |
| `../../quetz/tests/sysmode/firmware/riscv_virt_balar_fft.c` (+ `fft_firmware_data_256.h`) | sysmode guest |

## Prereqs (container — see quetz-docker/README-balar.md)
`nvcc`, GPGPU-Sim (`GPGPUSIM_ROOT`), `sst`, RV64 musl-gcc, `qemu-system-riscv64`.

## Phase A — trace replay
```sh
# 1. validate the algorithm on the host (no CUDA needed) and (re)gen artifacts
python3 balar_trace/fft_reference.py -n 256 --emit --outdir traces

# 2. build the FFT CUDA binary (PTX for GPGPU-Sim)
make -C balar_trace fft
# confirm the mangled kernel names match fft_reference.py's PTX_* constants:
cuobjdump -sass balar_trace/fft | grep -i 'fft_'

# 3. run the trace replay into balar + GPGPU-Sim
sst testQuetz-balar-fft.py --model-options=\
"-c gpu-v100-mem.cfg -s quetz_fft.stats -x ./balar_trace/fft -t traces/fft_impulse_256.trace -v 1"
sst testBalar-fft.py --model-options=\
"-c gpu-v100-mem.cfg -s balar_fft.stats -x ./balar_trace/fft -t traces/fft_impulse_256.trace -v 1"
```
Or via the suite: `sst-test-elements ... testsuite_default_balar.py`
(`test_quetz_balar_fft`, `test_balar_contract_fft`).

## Real-transform correctness (the tone check)
Impulse/DC are bit-exact but **twiddle-blind** — an FFT with broken twiddle math
would still pass `ratio==1.0`. To actually validate the GPU's complex-multiply,
run a tone `x[n]=cos(2π·k₀·n/N)` and tolerance-compare the GPU output to an
independent DFT:
```sh
# QuetzTestCPU/BalarTestCPU dump the GPU D2H result when FFT_DUMP=1
FFT_DUMP=1 sst testQuetz-balar-fft.py --model-options=\
"-c gpu-v100-mem.cfg -s tone.stats -x ./balar_trace/fft -t traces/fft_tone_256.trace -v 0"
python3 balar_trace/fft_reference.py -n 256 --compare cudamemcpyD2H-sim-*-size-2048.data --kind tone
# -> rel-L2 ~1e-7, peak bin = k0 (TONE_BIN)  => PASS
```

## Inverse FFT round-trip (self-consistent accuracy check)
`IFFT(FFT(x)) ≈ x` exercises every twiddle and needs no external reference — the
gold is the input itself. The inverse reuses the same kernels with conjugated
twiddles (`fft_twc`) plus an `fft_scale` by 1/N.
```sh
FFT_DUMP=1 sst testQuetz-balar-fft.py --model-options=\
"-c gpu-v100-mem.cfg -s rt.stats -x ./balar_trace/fft -t traces/fft_roundtrip_256.trace -v 0"
python3 balar_trace/fft_reference.py -n 256 --compare cudamemcpyD2H-sim-*-size-2048.data --kind tone --roundtrip
# -> recovery rel-L2 ~1e-7 (float)  => PASS
```

## Precision and accuracy-vs-N
- **Double precision:** `make -C balar_trace fft_double` builds the same kernels
  with `double2`; generate matching artifacts with `fft_reference.py --double`
  and point the SDL `-x` at `fft_double`. Recovery/forward error drops from
  ~1e-7 to ~1e-15 (precision/throughput tradeoff — double is much slower on
  GPGPU-Sim).
- **Accuracy vs N:** `run_fft_sweep.sh` reports the tone rel-L2 at each N
  alongside the perf stats (float32 error grows slowly, ~1.0e-7→1.5e-7 over
  N=256→16384, near the float32 floor).

## Phase B — Quetz QEMU sysmode guest
```sh
( cd ../../quetz/tests/sysmode/firmware && ./build.sh )   # builds riscv_virt_balar_fft
# run the sysmode FFT test (drives balar through the QEMU guest)
sst-test-elements ... testsuite_default_quetz.py   # test_quetz_balar_fft
```
Expect `Balar FFT Kernel_done correct_words=512/512` on the guest UART and a
much larger CUDA-API/flush count than vectorAdd (9 launches vs 1).

## Phase C — the comparison (the actual demonstration)
```sh
# staged vs single-shared-memory kernel (same FFT, different decomposition)
sst testQuetz-balar-fft.py --model-options="... -x ./balar_trace/fft -t traces/fft_impulse_256.trace" -s staged.stats   # staged main()
# (rebuild fft main() with mode=shared, or add a shared trace) -> shared.stats

python3 fft_stats_report.py --launches 9 \
    --run staged:staged.stats:staged.out \
    --run shared:shared.stats:shared.out
```
`fft_stats_report.py` tabulates the host↔GPU command traffic (cache writes,
doorbells, flushes, D2H bytes, per-launch), the D2H correctness gate, and the
GPGPU-Sim execution counters (cycles, instructions, IPC, DRAM) scraped from the
console log. Use it for three comparisons:
- **staged vs shared** — kernel decomposition shifts work from on-chip shared
  memory to DRAM and multiplies host command traffic.
- **N sweep** — `./run_fft_sweep.sh [N ...]` (default 256 512 1024) runs the
  staged FFT at each size and tabulates host-offload + GPU cost vs N in one shot.
- **trace vs sysmode** — the QEMU guest path adds the host marshalling/flush
  overhead that pure replay omits.

## Notes
- Sysmode scratch is 4 KiB; the staged FFT keeps intermediates on-device, so
  only one N-point H2D/D2H crosses the bus (N=256 → ~2.6 KiB packet+payload).
  Larger N is trace-path only unless `SCRATCH_BYTES` is raised.
- The firmware twiddle header is regenerated by `build.sh`; impulse correctness
  is twiddle-independent, but real twiddles keep the demo a faithful FFT.
