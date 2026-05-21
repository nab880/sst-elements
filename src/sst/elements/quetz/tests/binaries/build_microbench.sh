#!/bin/bash
# Build Quetz microbenchmark ELFs (RISC-V user-mode).
set -euo pipefail

SRC_DIR="$(cd "$(dirname "$0")/src" && pwd)"
OUT_DIR="$(cd "$(dirname "$0")" && pwd)"

CXX="${RISCV_CXX:-riscv64-linux-gnu-gcc}"
BASE=(-O2 -static -march=rv64gc -mabi=lp64d)

echo "=== Building microbenchmarks ==="
${CXX} "${BASE[@]}" -DSTRIDE=1  -o "${OUT_DIR}/stride_read_1"  "${SRC_DIR}/stride_read.c"
${CXX} "${BASE[@]}" -DSTRIDE=64 -o "${OUT_DIR}/stride_read_64" "${SRC_DIR}/stride_read.c"
${CXX} "${BASE[@]}" -o "${OUT_DIR}/pointer_chase" "${SRC_DIR}/pointer_chase.c"
${CXX} "${BASE[@]}" -o "${OUT_DIR}/write_burst"   "${SRC_DIR}/write_burst.c"
${CXX} "${BASE[@]}" -march=rv64gcv -o "${OUT_DIR}/rvv_vlen_sweep" "${SRC_DIR}/rvv_vlen_sweep.c"
${CXX} "${BASE[@]}" -fopenmp -o "${OUT_DIR}/false_share" "${SRC_DIR}/false_share.c"
echo "=== Done ==="
