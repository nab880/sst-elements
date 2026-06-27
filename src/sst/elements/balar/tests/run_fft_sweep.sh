#!/bin/bash
# run_fft_sweep.sh — FFT N-scaling sweep through DoorbellTestCPU + balar + GPGPU-Sim.
#
# Runs the staged radix-2 FFT (impulse vector) at several sizes and tabulates how
# the host<->GPU command traffic and GPGPU-Sim execution cost scale with N. Run
# inside the balar container (it sources the GPGPU-Sim environment like
# run-balar-tests.sh). Usage:  ./run_fft_sweep.sh [N ...]   (default 256 512 1024)
set -uo pipefail

SST_PREFIX="${SST_PREFIX:-/opt/sst}"
export PATH="${SST_PREFIX}/bin:/usr/local/cuda/bin:${PATH}"
export LD_LIBRARY_PATH="${SST_PREFIX}/lib:/usr/local/cuda/lib64:${LD_LIBRARY_PATH:-}"
export SST_HOME="${SST_PREFIX}"
export CUDA_INSTALL_PATH="${CUDA_INSTALL_PATH:-/usr/local/cuda}"
export CUDA_HOME="${CUDA_HOME:-/usr/local/cuda}"
export GPU_ARCH="${GPU_ARCH:-sm_70}"
export GPGPUSIM_ROOT="${GPGPUSIM_ROOT:-/opt/gpgpu-sim}"
set +u
source "${GPGPUSIM_ROOT}/setup_environment" sst >/dev/null 2>&1
set -u

TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "${TESTS_DIR}"

NS="${*:-256 512 1024}"
FFT_BIN="${TESTS_DIR}/balar_trace/fft"
[ -x "${FFT_BIN}" ] || make -C balar_trace fft

RUNS=()
ACC=()
for N in ${NS}; do
    python3 balar_trace/fft_reference.py -n "${N}" --emit --outdir traces >/dev/null
    out="/tmp/fft_sweep_${N}.out"
    stats="/tmp/fft_sweep_${N}.stats"
    echo "=== N=${N} (staged, impulse) ==="
    BALAR_CUDA_EXE_PATH="${FFT_BIN}" timeout 900 sst testDoorbellCPU-fft.py \
        --model-options="-c gpu-v100-mem.cfg -s ${stats} -x ${FFT_BIN} -t traces/fft_impulse_${N}.trace -v 0" \
        > "${out}" 2>&1
    ratio="$(grep correct_memD2H_ratio "${stats}" 2>/dev/null | grep -oE 'Min.f64 = [0-9.]+' | head -1)"
    echo "  exit=$?  ${ratio:-<no stats>}"
    RUNS+=(--run "N${N}:${stats}:${out}")

    # Numerical accuracy: tone vs independent DFT (impulse is bit-exact but
    # twiddle-blind). Dump the GPU output and tolerance-compare on the host.
    rm -f cudamemcpyD2H-sim-*.data
    FFT_DUMP=1 timeout 900 sst testDoorbellCPU-fft.py \
        --model-options="-c gpu-v100-mem.cfg -s /tmp/fft_tone_${N}.stats -x ${FFT_BIN} -t traces/fft_tone_${N}.trace -v 0" \
        > /tmp/fft_tone_${N}.out 2>&1
    dump="$(ls cudamemcpyD2H-sim-*-size-$((N * 8)).data 2>/dev/null | head -1)"
    acc="$(python3 balar_trace/fft_reference.py -n "${N}" --compare "${dump}" --kind tone 2>/dev/null | grep -oE 'rel-L2=[0-9.e+-]+' | head -1)"
    rm -f cudamemcpyD2H-*.data
    ACC+=("  N=${N}: ${acc:-<no dump>}")
done

echo ""
echo "######## FFT N-scaling sweep (staged radix-2, DoorbellTestCPU dual-link) ########"
python3 fft_stats_report.py "${RUNS[@]}"
echo ""
echo "######## Numerical accuracy vs N (tone, GPU float32 vs independent DFT) ########"
printf '%s\n' "${ACC[@]}"
