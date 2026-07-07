#!/bin/bash
# build.sh — build the expanded-coldfire-tests completeness firmware.
# Run from this directory (tests/sysmode/firmware/expanded/). Outputs
# binaries into this same directory. Shares coldfire_startup.S,
# link_m68k.ld, and the coldfire_*.h BSP headers with the main firmware
# build one level up -- NOT part of ./build.sh's normal firmware set.
set -e
FWDIR="$(cd "$(dirname "$0")" && pwd)"
cd "$FWDIR"

M68K_CC="${M68K_CC:-m68k-linux-gnu-gcc}"

M68K_FLAGS="-mcpu=5208 -O2 \
  -nostdlib -nostartfiles -ffreestanding \
  -I.. -T ../link_m68k.ld -Wl,--build-id=none"

for name in coldfire_xt_irq_burst coldfire_xt_stream_ack_early \
            coldfire_xt_be_alias coldfire_xt_wild_access \
            coldfire_xt_doorbell_flood coldfire_xt_scale_stress \
            coldfire_xt_irq_zero_latency coldfire_xt_dma_escape; do
    echo "=== expanded: ${name} ==="
    $M68K_CC $M68K_FLAGS ../coldfire_startup.S "${name}.c" -o "${name}"
    echo "  -> ${name}"
done

echo ""
echo "Expanded firmware built successfully."
ls -lh coldfire_xt_*  2>/dev/null | grep -v '\.c$'
