#!/bin/bash
# build.sh — build all system-emulation test firmware binaries.
# Run from this directory (tests/sysmode/firmware/).
# Outputs binaries into this same directory.
set -e
FWDIR="$(cd "$(dirname "$0")" && pwd)"
cd "$FWDIR"

RV64_CC="${RV64_CC:-/opt/riscv/bin/riscv64-unknown-linux-musl-gcc}"
ARM_CC="${ARM_CC:-arm-none-eabi-gcc}"
ARM_AS="${ARM_AS:-arm-none-eabi-as}"
ARM_LD="${ARM_LD:-arm-none-eabi-gcc}"
X86_CC="${X86_CC:-x86_64-linux-gnu-gcc}"
M68K_CC="${M68K_CC:-m68k-linux-gnu-gcc}"

RV64_FLAGS="-march=rv64gc -mabi=lp64d -O2 -mcmodel=medany \
  -nostdlib -nostartfiles -ffreestanding -mno-relax \
  -T link_rv64.ld -Wl,--build-id=none"

# NXP ColdFire MCF5208 (big-endian m68k), QEMU mcf5208evb.
M68K_FLAGS="-mcpu=5208 -O2 \
  -nostdlib -nostartfiles -ffreestanding \
  -T link_m68k.ld -Wl,--build-id=none"

ARM_CFLAGS="-mcpu=cortex-m7 -mthumb -O2 \
  -nostdlib -nostartfiles -ffreestanding"

X86_FLAGS="-m32 -ffreestanding -fno-stack-protector \
  -nostdlib -nostartfiles -O2 -T link_x86.ld -Wl,--build-id=none"

echo "=== RISC-V virt hello ==="
$RV64_CC $RV64_FLAGS riscv_virt_hello.c -o riscv_virt_hello
echo "  -> riscv_virt_hello"

echo "=== RISC-V virt UART echo ==="
$RV64_CC $RV64_FLAGS riscv_virt_uart_echo.c -o riscv_virt_uart_echo
echo "  -> riscv_virt_uart_echo"

echo "=== RISC-V virt MMIO poke ==="
$RV64_CC $RV64_FLAGS riscv_virt_mmio_poke.c -o riscv_virt_mmio_poke
echo "  -> riscv_virt_mmio_poke"

echo "=== RISC-V virt GPU trace ==="
$RV64_CC $RV64_FLAGS riscv_virt_gpu_trace.c -o riscv_virt_gpu_trace
echo "  -> riscv_virt_gpu_trace"

echo "=== RISC-V virt GPU kernel ==="
$RV64_CC $RV64_FLAGS riscv_virt_gpu_kernel.c -o riscv_virt_gpu_kernel
echo "  -> riscv_virt_gpu_kernel"

echo "=== RISC-V virt GPU async (P4 overlap demo) ==="
$RV64_CC $RV64_FLAGS riscv_virt_gpu_async.c -o riscv_virt_gpu_async
echo "  -> riscv_virt_gpu_async"

echo "=== RISC-V virt GPU async engine (P4 Quetz async-offload engine) ==="
$RV64_CC $RV64_FLAGS riscv_virt_gpu_async_engine.c -o riscv_virt_gpu_async_engine
echo "  -> riscv_virt_gpu_async_engine"

echo "=== RISC-V virt GPU async queue (P4 N-in-flight completion queue) ==="
$RV64_CC $RV64_FLAGS riscv_virt_gpu_async_queue.c -o riscv_virt_gpu_async_queue
echo "  -> riscv_virt_gpu_async_queue"

echo "=== RISC-V virt GPU FFT (synthetic-GPU timing, no balar) ==="
$RV64_CC $RV64_FLAGS riscv_virt_gpu_fft.c -o riscv_virt_gpu_fft
echo "  -> riscv_virt_gpu_fft"

echo "=== RISC-V virt GPU FFT OFFLOAD (device-computed; QuetzGpuDevice kernel_type=fft) ==="
$RV64_CC $RV64_FLAGS riscv_virt_gpu_fft_offload.c -o riscv_virt_gpu_fft_offload
echo "  -> riscv_virt_gpu_fft_offload"

echo "=== RISC-V virt Balar kernel ==="
$RV64_CC $RV64_FLAGS riscv_virt_balar_kernel.c -o riscv_virt_balar_kernel
echo "  -> riscv_virt_balar_kernel"

echo "=== RISC-V virt Balar async (P4 posted thread-sync) ==="
$RV64_CC $RV64_FLAGS riscv_virt_balar_async.c -o riscv_virt_balar_async
echo "  -> riscv_virt_balar_async"

echo "=== RISC-V virt Balar FFT ==="
# Regenerate the precomputed twiddle header if the generator is available.
FFT_REF="../../../../balar/tests/balar_trace/fft_reference.py"
if [ -f "$FFT_REF" ]; then
  python3 "$FFT_REF" -n 256 --header fft_firmware_data_256.h >/dev/null
fi
$RV64_CC $RV64_FLAGS riscv_virt_balar_fft.c -o riscv_virt_balar_fft
echo "  -> riscv_virt_balar_fft"

echo "=== ColdFire mcf5208evb hello (m68k) ==="
$M68K_CC $M68K_FLAGS coldfire_startup.S coldfire_hello.c -o coldfire_hello
echo "  -> coldfire_hello"

echo "=== ColdFire mcf5208evb serial monitor (m68k) ==="
$M68K_CC $M68K_FLAGS coldfire_startup.S coldfire_monitor.c -o coldfire_monitor
echo "  -> coldfire_monitor"

echo "=== ColdFire mcf5208evb balar vectorAdd (m68k) ==="
$M68K_CC $M68K_FLAGS coldfire_startup.S coldfire_gpu.c -o coldfire_gpu
echo "  -> coldfire_gpu"

echo "=== ColdFire mcf5208evb balar vectorAdd ASYNC (m68k, P4) ==="
$M68K_CC $M68K_FLAGS coldfire_startup.S coldfire_gpu_async.c -o coldfire_gpu_async
echo "  -> coldfire_gpu_async"

# Synthetic-GPU FFT (balar-free): CPU soft-float FFT timed by the synthetic GPU
# doorbell. ColdFire V2 has no FPU, so -msoft-float + libgcc (__*sf3 helpers).
# ColdFire V2 has no FPU and the m68k libgcc soft-float helpers (__mulsf3 &c.)
# hang on it, so the FFT uses Q16.16 integer fixed point (-DFFT_FIXED_POINT).
M68K_FP_FLAGS="-mcpu=5208 -O2 -DFFT_FIXED_POINT \
  -nostdlib -nostartfiles -ffreestanding \
  -T link_m68k.ld -Wl,--build-id=none"
# NO -lgcc: this m68k libgcc's helpers (__mulsf3, __muldi3, ...) hang on ColdFire
# V2. The firmware supplies its own __muldi3 (fft_muldi3.h) built from 32-bit
# multiplies, which is the only runtime helper the Q16.16 FFT needs.
echo "=== ColdFire mcf5208evb GPU FFT (synthetic, Q16.16 fixed-point, no balar) ==="
$M68K_CC $M68K_FP_FLAGS coldfire_startup.S coldfire_gpu_fft.c -o coldfire_gpu_fft
echo "  -> coldfire_gpu_fft"

# Device-computed FFT: no float/fixed-point math on the guest at all (raw u32
# bit-pattern compares), so plain M68K_FLAGS — no soft-float, no fft headers.
echo "=== ColdFire mcf5208evb GPU FFT OFFLOAD (device-computed) ==="
$M68K_CC $M68K_FLAGS coldfire_startup.S coldfire_gpu_fft_offload.c -o coldfire_gpu_fft_offload
echo "  -> coldfire_gpu_fft_offload"

echo "=== ARM Cortex-M7 hello ==="
$ARM_CC $ARM_CFLAGS -T link_arm_m7.ld -Wl,--build-id=none \
  arm_m7_startup.S arm_m7_hello.c -o arm_m7_hello
echo "  -> arm_m7_hello"

echo "=== x86 multiboot hello ==="
$X86_CC $X86_FLAGS x86_hello.c -o x86_hello
echo "  -> x86_hello"

echo "=== MIPS Malta hello (Python-generated raw binary) ==="
python3 gen_mips_hello.py
echo "  -> mips_malta_hello.bin"

echo ""
echo "All firmware binaries built successfully."
ls -lh riscv_virt_hello riscv_virt_uart_echo riscv_virt_mmio_poke \
        riscv_virt_gpu_trace riscv_virt_gpu_kernel riscv_virt_balar_kernel \
        riscv_virt_balar_fft \
        arm_m7_hello x86_hello mips_malta_hello.bin 2>/dev/null
