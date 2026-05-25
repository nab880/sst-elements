#!/bin/bash
# build.sh — user-mode GPU test binaries for Quetz usermode tests.
# Run from tests/usermode/sources/.  Outputs to tests/binaries/.
#
# Regenerate (needs Linux user ABI for qemu-riscv64, not bare-metal elf-gcc):
#   RV64_CC=/opt/riscv/bin/riscv64-unknown-linux-musl-gcc ./build.sh
# Docker image per TESTING.md; macOS riscv64-elf-gcc is not sufficient.
set -e
SRCDIR="$(cd "$(dirname "$0")" && pwd)"
# tests/binaries/ (not tests/usermode/binaries/)
BINDIR="$SRCDIR/../../binaries"
mkdir -p "$BINDIR"
BINDIR="$(cd "$BINDIR" && pwd)"
if [ -n "${RV64_CC:-}" ]; then
    CC="${RV64_CC}"
else
    for cand in \
        /opt/riscv/bin/riscv64-unknown-linux-musl-gcc \
        riscv64-linux-gnu-gcc \
        riscv64-unknown-linux-gnu-gcc; do
        if command -v "$cand" >/dev/null 2>&1; then
            CC="$cand"
            break
        fi
    done
fi
FLAGS="-static -O2 -Wall"

if [ -z "${CC:-}" ] || ! command -v "$CC" >/dev/null 2>&1; then
    echo "error: no RISC-V Linux cross compiler found (set RV64_CC)" >&2
    exit 1
fi
echo "Using CC=$CC"

echo "=== gpu_kernel_user ==="
"$CC" $FLAGS "$SRCDIR/gpu_kernel_user.c" -o "$BINDIR/gpu_kernel_user"
echo "  -> $BINDIR/gpu_kernel_user"

echo "=== gpu_trace_user ==="
"$CC" $FLAGS "$SRCDIR/gpu_trace_user.c" -o "$BINDIR/gpu_trace_user"
echo "  -> $BINDIR/gpu_trace_user"

echo ""
echo "User-mode GPU test binaries built successfully."
ls -lh "$BINDIR/gpu_kernel_user" "$BINDIR/gpu_trace_user"
