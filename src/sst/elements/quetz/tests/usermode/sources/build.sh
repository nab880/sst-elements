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

# --- P6 user-mode synchronous-MMIO programs ---------------------------------
# These drive the doorbell aperture through the linux-user SIGSEGV trap (needs an
# overlay-built qemu-<arch> with -sst-mmio-range). RV64 round-trips are built
# both non-compressed (rv64g, exercises the base decoder) and compressed
# (rv64gc, exercises the RVC decoder). The balar programs reuse the sysmode
# marshalling headers. m68k has no glibc-dev in the image, so build freestanding.
FW="$SRCDIR/../../sysmode/firmware"
BALAR_INC="$SRCDIR/../../../../balar"

echo "=== rv64_mmio_roundtrip (rv64g, non-compressed) ==="
"$CC" -static -O2 -march=rv64g -mabi=lp64d -Wall \
    "$SRCDIR/rv64_mmio_roundtrip.c" -o "$BINDIR/rv64_mmio_roundtrip"

echo "=== rv64_mmio_roundtrip_rvc (rv64gc, compressed) ==="
"$CC" -static -O2 -march=rv64gc -mabi=lp64d -Wall \
    "$SRCDIR/rv64_mmio_roundtrip.c" -o "$BINDIR/rv64_mmio_roundtrip_rvc"

echo "=== rv64_balar_user ==="
"$CC" -static -O2 -march=rv64gc -mabi=lp64d -Wall -I"$FW" -I"$BALAR_INC" \
    "$SRCDIR/rv64_balar_user.c" -o "$BINDIR/rv64_balar_user"

M68K_CC="${M68K_CC:-m68k-linux-gnu-gcc}"
if command -v "$M68K_CC" >/dev/null 2>&1; then
    M68K_FLAGS="-static -nostdlib -ffreestanding -O2 -Wall"
    echo "=== m68k_mmio_roundtrip ==="
    "$M68K_CC" $M68K_FLAGS "$SRCDIR/m68k_mmio_roundtrip.c" \
        -o "$BINDIR/m68k_mmio_roundtrip"
    echo "=== m68k_balar_user ==="
    "$M68K_CC" $M68K_FLAGS -I"$FW" "$SRCDIR/m68k_balar_user.c" \
        -o "$BINDIR/m68k_balar_user"
    echo "=== m68k_balar_async_user ==="
    "$M68K_CC" $M68K_FLAGS -I"$FW" "$SRCDIR/m68k_balar_async_user.c" \
        -o "$BINDIR/m68k_balar_async_user"
else
    echo "warning: $M68K_CC not found; skipping m68k P6 binaries" >&2
fi

echo ""
echo "User-mode GPU test binaries built successfully."
ls -lh "$BINDIR/gpu_kernel_user" "$BINDIR/gpu_trace_user"
