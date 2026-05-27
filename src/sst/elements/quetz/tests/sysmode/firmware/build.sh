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

RV64_FLAGS="-march=rv64gc -mabi=lp64d -O2 -mcmodel=medany \
  -nostdlib -nostartfiles -ffreestanding -mno-relax \
  -T link_rv64.ld -Wl,--build-id=none"

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

echo "=== RISC-V virt Balar kernel ==="
$RV64_CC $RV64_FLAGS riscv_virt_balar_kernel.c -o riscv_virt_balar_kernel
echo "  -> riscv_virt_balar_kernel"

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
        arm_m7_hello x86_hello mips_malta_hello.bin 2>/dev/null
