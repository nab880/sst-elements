#!/usr/bin/env python3
"""Generate the ColdFire/Raptor BSP-compat allowlist from a board contract.

The Raptor peripheral block allowlist used by the ``mcf-bsp-compat`` QEMU
device (``qemu-overlay/hw/misc/mcf_bsp_compat.c``) and the discovery
post-processor (``bsp_profile.py``) is derived from a single source of truth:
the board contract ``boards/raptor/board.json`` in the umbrella repository.

Any region in that contract carrying a ``bsp_compat`` object is an
allowlist member; its ``bsp_compat.block`` names the sparse block. Regions
without ``bsp_compat`` (RAM, QEMU-native UARTs, reserved apertures) are
excluded on purpose — the compat device only overlays otherwise-unmodeled
IPS space and must never shadow a native QEMU device.

This emits a checked-in C header so the QEMU build (which copies the overlay
verbatim, with no code-generation step) stays self-contained. A staleness
test regenerates and diffs, exactly like ``ADDRESS-MAP.md``.

Usage:
    gen_bsp_compat_blocks.py BOARD_JSON --write-header HEADER
    gen_bsp_compat_blocks.py BOARD_JSON --check-header HEADER
    gen_bsp_compat_blocks.py BOARD_JSON --write-dtimer-header HEADER
    gen_bsp_compat_blocks.py BOARD_JSON --check-dtimer-header HEADER
    gen_bsp_compat_blocks.py BOARD_JSON --write-gpio-header HEADER
    gen_bsp_compat_blocks.py BOARD_JSON --check-gpio-header HEADER
    gen_bsp_compat_blocks.py BOARD_JSON --print-json     # for bsp_profile.py
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def _as_int(value: object) -> int:
    return int(value, 0) if isinstance(value, str) else int(value)


# The compat-interface target token. Intentionally the stable family name
# ("raptor"), not the specific board variant (board["board"] may be
# "raptor-core2"), so the CLI/profile contract is not tied to a link layout.
COMPAT_TARGET = "raptor"


# bsp_compat.model selects which device owns a block. "compat" (the default
# when the key is absent) is a register-storage stub the mcf-bsp-compat device
# may overlay. The other models are dedicated QEMU devices that map their own
# regions — those blocks must be EXCLUDED from the compat allowlist so two
# devices never claim the same aperture, and are emitted into their own header:
#   "dtimer" -> mcf-dtimer  -> raptor_dtimer_blocks.h
#   "gpio"   -> mcf-gpio    -> raptor_gpio_blocks.h
COMPAT_MODEL = "compat"
DTIMER_MODEL = "dtimer"
GPIO_MODEL = "gpio"
KNOWN_MODELS = (COMPAT_MODEL, DTIMER_MODEL, GPIO_MODEL)

# Per dedicated-device-model header parameters: (guard, target-macro,
# struct name, array name, human-readable description of the model).
DEVICE_HEADERS = {
    DTIMER_MODEL: {
        "filename": "raptor_dtimer_blocks.h",
        "guard": "QUETZ_RAPTOR_DTIMER_BLOCKS_H",
        "target_macro": "RAPTOR_DTIMER_TARGET",
        "struct": "RaptorDtimerBlockDesc",
        "array": "raptor_dtimer_blocks",
        "blurb": ("These are the DMA-timer modules (DTIM0-3) the mcf-dtimer "
                  "device maps and drives with a virtual-time counter."),
    },
    GPIO_MODEL: {
        "filename": "raptor_gpio_blocks.h",
        "guard": "QUETZ_RAPTOR_GPIO_BLOCKS_H",
        "target_macro": "RAPTOR_GPIO_TARGET",
        "struct": "RaptorGpioBlockDesc",
        "array": "raptor_gpio_blocks",
        "blurb": ("These are the GPIO banks (GPIOB0-3) the mcf-gpio device "
                  "maps: 16-bit registers, mask-in-high-byte value writes, "
                  "read-modify-write direction, readback-correct."),
    },
}


def extract_blocks(board: dict,
                   model: str | None = COMPAT_MODEL,
                   ) -> list[tuple[str, int, int]]:
    """Return sorted (name, base, size) for bsp_compat regions.

    ``model`` filters by ``bsp_compat.model`` (default "compat" for absent).
    Pass ``model=None`` to return every bsp_compat block regardless of model.
    """
    blocks: dict[str, tuple[int, int]] = {}
    for region in board.get("regions", []):
        compat = region.get("bsp_compat")
        if not compat:
            continue
        name = compat.get("block")
        if not isinstance(name, str) or not name:
            raise ValueError(
                f"region {region.get('name')!r}: bsp_compat.block must be a "
                "non-empty string"
            )
        region_model = compat.get("model", COMPAT_MODEL)
        if region_model not in KNOWN_MODELS:
            raise ValueError(
                f"region {region.get('name')!r}: bsp_compat.model "
                f"{region_model!r} is not one of {KNOWN_MODELS}"
            )
        if model is not None and region_model != model:
            continue
        base = _as_int(region["base"])
        size = _as_int(region["size"])
        if name in blocks:
            raise ValueError(f"duplicate bsp_compat block name {name!r}")
        blocks[name] = (base, size)
    return [(name, base, size) for name, (base, size) in
            sorted(blocks.items(), key=lambda item: item[1][0])]


def render_header(board: dict) -> str:
    target = COMPAT_TARGET
    blocks = extract_blocks(board, model=COMPAT_MODEL)
    if not blocks:
        raise ValueError("board contract declares no bsp_compat regions")
    lines = [
        "/*",
        " * raptor_bsp_blocks.h -- GENERATED; do not edit.",
        " *",
        " * Regenerate with:",
        " *   tools/gen_bsp_compat_blocks.py boards/raptor/board.json \\",
        " *       --write-header qemu-overlay/hw/misc/raptor_bsp_blocks.h",
        " *",
        " * Source of truth: boards/raptor/board.json (regions carrying a",
        " * bsp_compat object with model \"compat\" or none). This is the",
        " * sparse-block allowlist the mcf-bsp-compat device may overlay on",
        " * otherwise-unmodeled IPS space. Blocks owned by a dedicated device",
        " * model (bsp_compat.model \"dtimer\" or \"gpio\") are emitted separately",
        " * and excluded here so two devices never claim the same aperture.",
        " */",
        "",
        "#ifndef QUETZ_RAPTOR_BSP_BLOCKS_H",
        "#define QUETZ_RAPTOR_BSP_BLOCKS_H",
        "",
        "#include <stdint.h>",
        "",
        f'#define RAPTOR_BSP_COMPAT_TARGET "{target}"',
        "",
        "typedef struct RaptorBspBlockDesc {",
        "    const char *name;",
        "    uint64_t base;",
        "    uint64_t size;",
        "} RaptorBspBlockDesc;",
        "",
        "static const RaptorBspBlockDesc raptor_bsp_blocks[] = {",
    ]
    width = max(len(name) for name, _, _ in blocks)
    for name, base, size in blocks:
        quoted = f'"{name}",'
        lines.append(
            f"    {{ {quoted:<{width + 3}} 0x{base:08x}, 0x{size:x} }},"
        )
    lines += [
        "};",
        "",
        "#endif /* QUETZ_RAPTOR_BSP_BLOCKS_H */",
        "",
    ]
    return "\n".join(lines)


def render_device_header(board: dict, model: str) -> str:
    """Emit the block table owned by a dedicated device model (dtimer/gpio)."""
    spec = DEVICE_HEADERS[model]
    blocks = extract_blocks(board, model=model)
    if not blocks:
        raise ValueError(
            f"board contract declares no bsp_compat regions with model {model!r}"
        )
    lines = [
        "/*",
        f" * {spec['filename']} -- GENERATED; do not edit.",
        " *",
        " * Regenerate with:",
        f" *   tools/gen_bsp_compat_blocks.py boards/raptor/board.json \\",
        f" *       --write-{model}-header qemu-overlay/hw/misc/{spec['filename']}",
        " *",
        " * Source of truth: boards/raptor/board.json (regions carrying a",
        f" * bsp_compat object with model \"{model}\"). {spec['blurb']}",
        " * They are deliberately excluded from the mcf-bsp-compat allowlist",
        " * in raptor_bsp_blocks.h so two devices never claim the same aperture.",
        " */",
        "",
        f"#ifndef {spec['guard']}",
        f"#define {spec['guard']}",
        "",
        "#include <stdint.h>",
        "",
        f'#define {spec["target_macro"]} "{COMPAT_TARGET}"',
        "",
        f"typedef struct {spec['struct']} {{",
        "    const char *name;",
        "    uint64_t base;",
        "    uint64_t size;",
        f"}} {spec['struct']};",
        "",
        f"static const {spec['struct']} {spec['array']}[] = {{",
    ]
    width = max(len(name) for name, _, _ in blocks)
    for name, base, size in blocks:
        quoted = f'"{name}",'
        lines.append(
            f"    {{ {quoted:<{width + 3}} 0x{base:08x}, 0x{size:x} }},"
        )
    lines += [
        "};",
        "",
        f"#endif /* {spec['guard']} */",
        "",
    ]
    return "\n".join(lines)


def load_board(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("board", type=Path)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--write-header", type=Path)
    group.add_argument("--check-header", type=Path)
    group.add_argument("--write-dtimer-header", type=Path)
    group.add_argument("--check-dtimer-header", type=Path)
    group.add_argument("--write-gpio-header", type=Path)
    group.add_argument("--check-gpio-header", type=Path)
    group.add_argument("--print-json", action="store_true")
    args = parser.parse_args(argv)

    # Map the per-model header options to (model, path, is_write).
    device_opts = [
        (DTIMER_MODEL, args.write_dtimer_header, True),
        (DTIMER_MODEL, args.check_dtimer_header, False),
        (GPIO_MODEL, args.write_gpio_header, True),
        (GPIO_MODEL, args.check_gpio_header, False),
    ]

    try:
        board = load_board(args.board)
        if args.print_json:
            # Discovery (bsp_profile.py) wants every bsp_compat block regardless
            # of which device models it, so it can still name DTIM/GPIO accesses.
            blocks = extract_blocks(board, model=None)
            print(json.dumps(
                {"target": COMPAT_TARGET,
                 "blocks": {name: [base, size] for name, base, size in blocks}},
                indent=2,
            ))
            return 0
        target_path = None
        is_write = False
        for model, path, write in device_opts:
            if path is not None:
                rendered = render_device_header(board, model)
                target_path = path
                is_write = write
                break
        else:
            rendered = render_header(board)
            target_path = args.write_header or args.check_header
            is_write = args.write_header is not None
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as exc:
        print(f"gen_bsp_compat_blocks: {exc}", file=sys.stderr)
        return 1

    if is_write:
        target_path.write_text(rendered, encoding="utf-8")
    else:
        try:
            current = target_path.read_text(encoding="utf-8")
        except OSError as exc:
            print(f"gen_bsp_compat_blocks: {exc}", file=sys.stderr)
            return 1
        if current != rendered:
            print(
                f"gen_bsp_compat_blocks: {target_path} is stale; "
                f"regenerate from {args.board}",
                file=sys.stderr,
            )
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
