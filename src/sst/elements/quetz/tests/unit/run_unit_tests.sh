#!/bin/bash
# Build and run Quetz C++ unit tests (no QEMU/SST simulation).
set -euo pipefail

UNIT_DIR="$(cd "$(dirname "$0")" && pwd)"
QUETZ_DIR="$(cd "${UNIT_DIR}/../.." && pwd)"
BUILD_DIR="${UNIT_DIR}/_build"
mkdir -p "${BUILD_DIR}"

SST_HOME="${SST_HOME:-/opt/sst}"
SST_INC="${SST_HOME}/include"

if [ ! -d "${SST_INC}/sst" ]; then
    CFG="$(command -v sst-config 2>/dev/null || true)"
    if [ -n "${CFG}" ]; then
        SST_HOME="$("${CFG}" --prefix)"
        SST_INC="${SST_HOME}/include"
    fi
fi

CXX="${CXX:-g++}"
CXXFLAGS=(
    -std=c++17
    -Wall
    -Wextra
    -I"${UNIT_DIR}"
    -I"${QUETZ_DIR}"
    -I"${SST_INC}"
)

compile() {
    local src="$1"
    local out="$2"
    shift 2
    echo "  compile ${src##*/}"
    "${CXX}" "${CXXFLAGS[@]}" "$@" -o "${BUILD_DIR}/${out}" "${UNIT_DIR}/${src}"
}

compile test_smoke.cc test_smoke
compile test_ipc_layout.cc test_ipc_layout
compile test_decoder_riscv.cc test_decoder_riscv
compile test_decoder_aarch64.cc test_decoder_aarch64
compile test_decoder_generic.cc test_decoder_generic
compile test_region_table.cc test_region_table
compile test_mem_issue_split.cc test_mem_issue_split
compile test_quetz_config.cc test_quetz_config
compile test_command_buffer.cc test_command_buffer
compile test_config_manager.cc test_config_manager \
    "${QUETZ_DIR}/quetz_platform_profiles.cc"

TESTS=(
    test_smoke
    test_ipc_layout
    test_decoder_riscv
    test_decoder_aarch64
    test_decoder_generic
    test_region_table
    test_mem_issue_split
    test_quetz_config
    test_command_buffer
    test_config_manager
)

echo "=== Quetz unit tests ==="
for t in "${TESTS[@]}"; do
    echo "==> ${t}"
    "${BUILD_DIR}/${t}"
done
echo "=== All Quetz unit tests passed ==="
