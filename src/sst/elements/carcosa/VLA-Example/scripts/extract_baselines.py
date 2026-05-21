#!/usr/bin/env python3
"""Average Phase1 profile CSV (cpu/gpu,...delta_ps) -> 18-value baseline_ps CSV for Phase2 delay agents."""
import argparse
import sys
import re
from collections import defaultdict

NUM_KERNELS = 18

KERNEL_NAMES = [
    "IDLE", "VISION_INGESTION", "PATCHIFICATION_EMBED",
    "VIS_ATTN_PROJ", "GLOBAL_SPATIAL_ATTN", "VIS_FFN",
    "MLP_PROJECTOR", "SEQ_CONCAT", "PREFILL_ATTN_PROJ",
    "PREFILL_CAUSAL_ATTN", "PREFILL_FFN", "GEMV_PROJECT",
    "KV_CACHE_ATTN", "DECODE_FFN", "LM_HEAD",
    "DETOK_DEQUANT", "FAST_IDCT", "ACTUATE",
]


def parse_profile(text):
    cpu_deltas = defaultdict(list)
    gpu_deltas = defaultdict(list)

    pattern = re.compile(
        r"^(cpu|gpu),(\d+),[^,]+,(\d+),(\d+),(\d+)$", re.MULTILINE
    )
    for m in pattern.finditer(text):
        core = m.group(1)
        kid = int(m.group(2))
        delta = int(m.group(5))
        if core == "cpu":
            cpu_deltas[kid].append(delta)
        else:
            gpu_deltas[kid].append(delta)

    return cpu_deltas, gpu_deltas


def compute_averages(deltas):
    avgs = []
    for i in range(NUM_KERNELS):
        vals = deltas.get(i, [])
        avgs.append(int(sum(vals) / len(vals)) if vals else 0)
    return avgs


def read_input(path):
    if path == "-":
        return sys.stdin.read()
    with open(path) as f:
        return f.read()


def audit_averages(cpu_avgs, gpu_avgs):
    """Return (zero_cpu, zero_gpu) lists of (kernel_id, kernel_name) with 0 ps."""
    zero_cpu = [(i, KERNEL_NAMES[i]) for i in range(NUM_KERNELS) if cpu_avgs[i] == 0]
    zero_gpu = [(i, KERNEL_NAMES[i]) for i in range(NUM_KERNELS) if gpu_avgs[i] == 0]
    return zero_cpu, zero_gpu


def main():
    ap = argparse.ArgumentParser(
        description="Extract Phase 1 per-kernel baselines for Phase 2 delay agents."
    )
    ap.add_argument(
        "input_file",
        help="Phase 1 log (or '-' for stdin)",
    )
    ap.add_argument(
        "--emit-env",
        action="store_true",
        help="Print only export lines for eval in shell (no table).",
    )
    ap.add_argument(
        "--audit",
        action="store_true",
        help="Audit per-kernel averages and exit 1 if any CPU or GPU slot is zero.",
    )
    args = ap.parse_args()

    text = read_input(args.input_file)

    cpu_deltas, gpu_deltas = parse_profile(text)

    if not cpu_deltas and not gpu_deltas:
        print("ERROR: No profile data found in input.", file=sys.stderr)
        sys.exit(1)

    cpu_avgs = compute_averages(cpu_deltas)
    gpu_avgs = compute_averages(gpu_deltas)

    cpu_csv = ",".join(str(v) for v in cpu_avgs)
    gpu_csv = ",".join(str(v) for v in gpu_avgs)

    if args.audit:
        zero_cpu, zero_gpu = audit_averages(cpu_avgs, gpu_avgs)
        if not zero_cpu and not zero_gpu:
            print(f"Baseline audit OK: all {NUM_KERNELS} kernels have non-zero "
                  f"CPU and GPU baselines.")
            return
        print("Baseline audit FAILED:", file=sys.stderr)
        if zero_cpu:
            print(f"  CPU baseline is zero for {len(zero_cpu)}/{NUM_KERNELS} kernels:",
                  file=sys.stderr)
            for i, name in zero_cpu:
                print(f"    kernel_id={i:2d}  {name}", file=sys.stderr)
        if zero_gpu:
            print(f"  GPU baseline is zero for {len(zero_gpu)}/{NUM_KERNELS} kernels:",
                  file=sys.stderr)
            for i, name in zero_gpu:
                print(f"    kernel_id={i:2d}  {name}", file=sys.stderr)
        print("\nHint: ensure Phase 1 ran to completion (clear VLA_SST_STOP_AT) and that\n"
              "the FSM knobs (VLA_MAX_CYCLES, VLA_NUM_VIT_LAYERS, VLA_NUM_LLM_LAYERS,\n"
              "VLA_NUM_ACTION_TOKENS) drive every state in vla-fsm.cc at least once.",
              file=sys.stderr)
        sys.exit(1)

    if args.emit_env:
        print(f'export VLA_BASELINE_CPU_PS="{cpu_csv}"')
        print(f'export VLA_BASELINE_GPU_PS="{gpu_csv}"')
        return

    print("=== Per-Kernel Baseline Averages (picoseconds) ===\n")
    print(f"{'ID':>3}  {'Kernel':<24}  {'CPU avg ps':>16}  {'GPU avg ps':>16}  {'CPU N':>5}  {'GPU N':>5}")
    print("-" * 80)
    for i in range(NUM_KERNELS):
        cn = len(cpu_deltas.get(i, []))
        gn = len(gpu_deltas.get(i, []))
        print(f"{i:3d}  {KERNEL_NAMES[i]:<24}  {cpu_avgs[i]:>16,}  {gpu_avgs[i]:>16,}  {cn:>5}  {gn:>5}")

    print("\n=== Environment Variables for Phase 2 ===\n")
    print(f"export VLA_BASELINE_CPU_PS=\"{cpu_csv}\"")
    print(f"export VLA_BASELINE_GPU_PS=\"{gpu_csv}\"")

    print("\n=== Quick-Run Command (run from tests/ with SST env) ===\n")
    print(f"VLA_BASELINE_CPU_PS=\"{cpu_csv}\" \\")
    print(f"VLA_BASELINE_GPU_PS=\"{gpu_csv}\" \\")
    print("VLA_SCALE_FACTOR=1.0 \\")
    print("sst testCarcosaVLA_GPUCPU_Synth.py")


if __name__ == "__main__":
    main()
