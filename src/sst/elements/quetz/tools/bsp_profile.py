#!/usr/bin/env python3
"""Convert a Raptor BSP-compat JSONL trace into diagnostics and a safe profile."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
from collections import Counter, defaultdict
from pathlib import Path


# The block allowlist is the single source of truth in the board contract
# (boards/raptor/board.json, regions carrying `bsp_compat`). It is loaded at
# runtime rather than hardcoded so this tool cannot drift from the QEMU
# device's allowlist (raptor_bsp_blocks.h, generated from the same contract).
#
# Resolution order for the contract:
#   1. an explicit --board path;
#   2. boards/raptor/board.json walked up from this file (umbrella checkout);
#   3. fall back to deriving block bases from the trace's own records.
_DEFAULT_BOARD_NAMES = ("boards/raptor/board.json",)


def _find_board() -> Path | None:
    here = Path(__file__).resolve()
    for parent in here.parents:
        for rel in _DEFAULT_BOARD_NAMES:
            candidate = parent / rel
            if candidate.is_file():
                return candidate
    return None


def load_blocks(board_path: Path | None) -> dict[str, tuple[int, int]]:
    """Return {block_name: (base, size)} from the board contract, or {}."""
    path = board_path or _find_board()
    if not path or not path.is_file():
        return {}
    try:
        board = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    blocks: dict[str, tuple[int, int]] = {}
    for region in board.get("regions", []):
        compat = region.get("bsp_compat")
        if not compat:
            continue
        name = compat.get("block")
        base = region["base"]
        size = region["size"]
        blocks[name] = (_number(base), _number(size))
    return blocks


def _number(value: object) -> int:
    return value if isinstance(value, int) else int(str(value), 0)


def load_trace(path: Path) -> list[dict]:
    records = []
    with path.open(encoding="utf-8") as stream:
        for lineno, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                item = json.loads(line)
                item["pc_int"] = _number(item["pc"])
                item["address_int"] = _number(item["address"])
                item["value_int"] = _number(item["value"])
                records.append(item)
            except (KeyError, TypeError, ValueError, json.JSONDecodeError) as exc:
                raise ValueError(f"{path}:{lineno}: invalid BSP trace record: {exc}") from exc
    return records


def resolve_sources(records: list[dict], elf: Path | None) -> dict[int, tuple[str, str]]:
    pcs = sorted({r["pc_int"] for r in records if r["pc_int"]})
    if not elf or not elf.is_file() or not pcs:
        return {}
    tool = shutil.which("m68k-linux-gnu-addr2line") or shutil.which("addr2line")
    if not tool:
        return {}
    cmd = [tool, "-f", "-C", "-e", str(elf), *[hex(pc) for pc in pcs]]
    try:
        result = subprocess.run(cmd, check=True, capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError):
        return {}
    lines = result.stdout.splitlines()
    if len(lines) < len(pcs) * 2:
        return {}
    return {pc: (lines[i * 2], lines[i * 2 + 1]) for i, pc in enumerate(pcs)}


def enrich_trace(records: list[dict], sources: dict[int, tuple[str, str]], path: Path) -> None:
    with path.open("w", encoding="utf-8") as stream:
        for original in records:
            item = {k: v for k, v in original.items() if not k.endswith("_int")}
            if original["pc_int"] in sources:
                item["function"], item["source"] = sources[original["pc_int"]]
            stream.write(json.dumps(item, sort_keys=True) + "\n")


def _location(record: dict, sources: dict[int, tuple[str, str]]) -> str:
    symbol = sources.get(record["pc_int"])
    return f"{symbol[0]} at {symbol[1]}" if symbol else record["pc"]


def analyze(records: list[dict], sources: dict[int, tuple[str, str]],
            poll_threshold: int = 100) -> str:
    access_counts = Counter((r["block"], r["address_int"], r["size"], r["op"])
                            for r in records)
    unknown_counts = Counter((r["block"], r["address_int"], r["size"], r["op"])
                             for r in records if not r.get("known", False))
    poll_runs = []
    run_key = None
    run_count = 0
    run_record = None
    for record in records + [{"op": "end"}]:
        key = ((record.get("pc_int"), record.get("address_int"), record.get("size"))
               if record.get("op") == "read" else None)
        if key == run_key and key is not None:
            run_count += 1
        else:
            if run_key is not None and run_count >= poll_threshold and run_record:
                poll_runs.append((run_count, run_record))
            run_key = key
            run_count = 1 if key is not None else 0
            run_record = record if key is not None else None

    last_write = {}
    mismatches = []
    for record in records:
        key = (record["address_int"], record["size"])
        if record["op"] == "write":
            last_write[key] = record
        elif key in last_write and record["value_int"] != last_write[key]["value_int"]:
            mismatches.append((last_write.pop(key), record))

    lines = ["Raptor BSP compatibility discovery", f"accesses: {len(records)}",
             f"unique access shapes: {len(access_counts)}",
             f"unknown profile access shapes: {len(unknown_counts)}", "",
             "Likely status polls:"]
    if poll_runs:
        for count, record in sorted(poll_runs, reverse=True, key=lambda x: x[0]):
            lines.append(f"  {count} reads at {record['address']} size={record['size']} "
                         f"from {_location(record, sources)}; last value={record['value']}")
    else:
        lines.append("  none detected")
    lines.extend(["", "Write/readback mismatches:"])
    if mismatches:
        for write, read in mismatches[:50]:
            lines.append(f"  {write['address']} size={write['size']}: wrote {write['value']}, "
                         f"read {read['value']} at {_location(read, sources)}")
    else:
        lines.append("  none detected")
    lines.extend(["", "Unknown registers (defaulted to RAZ/WI):"])
    if unknown_counts:
        for (block, address, size, op), count in sorted(unknown_counts.items()):
            lines.append(f"  {block} {address:#010x} size={size} {op}: {count}")
    else:
        lines.append("  none")
    lines.extend(["", "Generated profiles are intentionally inert. Set documented reset/status",
                  "bits and writable masks; do not use guessed values as hardware truth."])
    return "\n".join(lines) + "\n"


def make_profile(records: list[dict], blocks: dict[str, tuple[int, int]] | None = None) -> dict:
    blocks = blocks or {}
    observed = defaultdict(set)
    block_min_addr: dict[str, int] = {}
    for record in records:
        name = record["block"]
        observed[name].add((record["address_int"], int(record["size"])))
        addr = record["address_int"]
        block_min_addr[name] = min(block_min_addr.get(name, addr), addr)
    out_blocks = []
    # Emit observed blocks in canonical (base) order when known, else by name.
    def _base_of(name: str) -> int:
        if name in blocks:
            return blocks[name][0]
        # Fall back to the block's lowest observed address aligned to 0x4000.
        return block_min_addr.get(name, 0) & ~0x3FFF

    for name in sorted(observed, key=lambda n: (_base_of(n), n)):
        base, size = blocks.get(name, (_base_of(name), 0x4000))
        registers = []
        for address, width in sorted(observed[name]):
            offset = address - base
            registers.append({"name": f"reg_{offset:04x}_{width * 8}",
                              "offset": f"0x{offset:x}", "width": width,
                              "reset": "0x0", "writable_mask": "0x0"})
        out_blocks.append({"name": name, "base": f"0x{base:x}",
                       "size": f"0x{size:x}", "registers": registers})
    return {"version": 1, "target": "raptor", "blocks": out_blocks}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--trace-out", required=True, type=Path)
    parser.add_argument("--report-out", required=True, type=Path)
    parser.add_argument("--profile-out", required=True, type=Path)
    parser.add_argument("--elf", type=Path)
    parser.add_argument("--board", type=Path,
                        help="board contract (default: locate boards/raptor/board.json)")
    parser.add_argument("--poll-threshold", type=int, default=100)
    args = parser.parse_args()
    records = load_trace(args.input)
    blocks = load_blocks(args.board)
    sources = resolve_sources(records, args.elf)
    enrich_trace(records, sources, args.trace_out)
    args.report_out.write_text(analyze(records, sources, args.poll_threshold), encoding="utf-8")
    args.profile_out.write_text(json.dumps(make_profile(records, blocks), indent=2) + "\n",
                                encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
