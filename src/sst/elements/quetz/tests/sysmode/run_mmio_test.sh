#!/usr/bin/env bash
# run_mmio_test.sh — local runner for basic_quetz_mmio.py (P0 sanity test).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FW_DIR="${SCRIPT_DIR}/firmware"
TEST_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

SST_CORE_PREFIX="${SST_CORE_PREFIX:-${SST_HOME:-}}"
if [[ -z "${SST_CORE_PREFIX}" ]]; then
  if command -v sst-config >/dev/null 2>&1; then
    SST_CORE_PREFIX="$(sst-config --prefix)"
  else
    echo "Set SST_HOME or SST_CORE_PREFIX to your SST install prefix." >&2
    exit 1
  fi
fi

SST_ELEMENTS_PREFIX="${SST_ELEMENTS_PREFIX:-${SST_CORE_PREFIX}}"
export PATH="${SST_CORE_PREFIX}/bin:${PATH}"

FW="${FW_DIR}/riscv_virt_mmio_poke"
if [[ ! -x "${FW}" ]]; then
  echo "Building firmware (riscv_virt_mmio_poke)..."
  RV64_CC="${RV64_CC:-$(command -v riscv64-elf-gcc || true)}"
  if [[ -z "${RV64_CC}" ]]; then
    echo "Install riscv64-elf-gcc (e.g. brew install riscv64-elf-gcc) or set RV64_CC." >&2
    exit 1
  fi
  (cd "${FW_DIR}" && RV64_CC="${RV64_CC}" ./build.sh)
fi

PLUGIN="${SST_ELEMENTS_PREFIX}/libexec/libqemu_sst_plugin.so"
if [[ ! -f "${PLUGIN}" ]]; then
  PLUGIN="${QUETZ_PLUGIN:-}"
fi
if [[ -z "${PLUGIN}" || ! -f "${PLUGIN}" ]]; then
  echo "libqemu_sst_plugin.so not found; build quetz and copy to \${prefix}/libexec/." >&2
  exit 1
fi

QEMU="${QUETZ_QEMU:-$(command -v qemu-system-riscv64 || true)}"
if [[ -z "${QEMU}" ]]; then
  echo "qemu-system-riscv64 not found (brew install qemu)." >&2
  exit 1
fi

OUT="${OUT:-/tmp/quetz_mmio_test}"
mkdir -p "${OUT}"

export SST_HOME="${SST_CORE_PREFIX}"
export QUETZ_EXE="${FW}"
export QUETZ_QEMU="${QEMU}"
export QUETZ_PLUGIN="${PLUGIN}"
export QUETZ_QEMU_ARGS="-machine virt -nographic -bios none"
export QUETZ_LOADER="-kernel"
export QUETZ_RAM_START=0
export QUETZ_RAM_END=0xFFFFFFFF
export QUETZ_MMIO_START=0x80100000
export QUETZ_MMIO_END=0x801003FF
export QUETZ_REGION_HANDLER_COUNT=1
export QUETZ_REGION_HANDLER0_TYPE=filtered
export QUETZ_REGION_HANDLER0_START=0
export QUETZ_REGION_HANDLER0_END=0x7FFFFFFF

echo "Running basic_quetz_mmio.py (output: ${OUT}/mmio.out)"
cd "${OUT}"
sst "${SCRIPT_DIR}/basic_quetz_mmio.py" 2>&1 | tee mmio.out

echo ""
echo "Key stats:"
grep -E 'mmio_write_requests|write_requests\.0' mmio.out || true
