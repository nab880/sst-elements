# Build the Phase 1 real-binary and Phase 2 stub RISC-V Linux ELF workloads.
#
# Phase 1 (vla_cpu, vla_gpu): run the real CPU/GPU kernels on Vanadis with the
# Hyades MMIO control loop; the binaries also publish four labeled regions
# (weights/kv_cache/activations/action_queue) via the HYADES_REGION_* ABI and
# write a per-frame action checksum so EccGuard's region-aware policy can
# target faults and ActionScorer can detect propagation. See vla_cpu.c.
#
# Phase 2 (stub_cpu, stub_gpu): memory-traffic generators driving the same
# Hyades + region-publish ABI without the real kernels. See stub_cpu.c.
#
# Both targets link against musl via Zig's bundled cross-compilation
# toolchain, so a fresh macOS or Linux box can rebuild them without an
# external riscv64-unknown-linux-* GNU toolchain.
#
# Usage:
#   make                # rebuild all four binaries (Phase 1 + Phase 2)
#   make phase1         # vla_cpu + vla_gpu only
#   make phase2         # stub_cpu + stub_gpu only
#   make vla_cpu        # one at a time
#   make clean
#
# Override the toolchain via CROSS_CC, e.g.:
#   make CROSS_CC="riscv64-unknown-linux-musl-gcc"   # native musl
#   make CROSS_CC="riscv64-unknown-linux-gnu-gcc"    # glibc
# Extra flags can be appended with CFLAGS_EXTRA.

CARCOSA_INC := ../..

# Default cross-compiler: Zig's drop-in clang frontend targeting
# riscv64-linux-musl. zig bundles musl headers + libs, so no external sysroot
# is required. Tested with zig 0.16.
CROSS_CC ?= zig cc -target riscv64-linux-musl
CFLAGS   ?= -static -O2 -I$(CARCOSA_INC) $(CFLAGS_EXTRA)

STUBS    := stub_cpu stub_gpu
VLA_BINS := vla_cpu vla_gpu

.PHONY: all phase1 phase2 clean
all: $(VLA_BINS) $(STUBS)
phase1: $(VLA_BINS)
phase2: $(STUBS)

stub_%: stub_%.c $(CARCOSA_INC)/hyades.h
	$(CROSS_CC) $(CFLAGS) -o $@ $<
	@file $@

vla_%: vla_%.c vla_shared.h $(CARCOSA_INC)/hyades.h
	$(CROSS_CC) $(CFLAGS) -lm -o $@ $<
	@file $@

clean:
	rm -f $(STUBS) $(VLA_BINS)
