#!/usr/bin/env python3
"""Convert an mcf-bsp-compat JSONL trace into diagnostics and a safe profile."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
from collections import Counter, defaultdict
from pathlib import Path


MCF5208_BLOCKS = {
    "scm": (0xFC000000, 0x4000), "xbs": (0xFC004000, 0x4000),
    "fbcs": (0xFC008000, 0x4000), "scm2": (0xFC040000, 0x4000),
    "edma": (0xFC044000, 0x4000), "i2c": (0xFC058000, 0x4000),
    "qspi": (0xFC05C000, 0x4000), "dtim0": (0xFC070000, 0x4000),
    "dtim1": (0xFC074000, 0x4000), "eport": (0xFC088000, 0x4000),
    "watchdog": (0xFC08C000, 0x4000), "pll": (0xFC090000, 0x4000),
    "wtm": (0xFC098000, 0x4000), "gpio": (0xFC0A4000, 0x4000),
}


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

    lines = ["MCF5208 BSP compatibility discovery", f"accesses: {len(records)}",
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


def make_profile(records: list[dict]) -> dict:
    observed = defaultdict(set)
    for record in records:
        observed[record["block"]].add((record["address_int"], int(record["size"])))
    blocks = []
    for name, (base, size) in MCF5208_BLOCKS.items():
        if name not in observed:
            continue
        registers = []
        for address, width in sorted(observed[name]):
            offset = address - base
            registers.append({"name": f"reg_{offset:04x}_{width * 8}",
                              "offset": f"0x{offset:x}", "width": width,
                              "reset": "0x0", "writable_mask": "0x0"})
        blocks.append({"name": name, "base": f"0x{base:x}",
                       "size": f"0x{size:x}", "registers": registers})
    return {"version": 1, "target": "mcf5208", "blocks": blocks}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--trace-out", required=True, type=Path)
    parser.add_argument("--report-out", required=True, type=Path)
    parser.add_argument("--profile-out", required=True, type=Path)
    parser.add_argument("--elf", type=Path)
    parser.add_argument("--poll-threshold", type=int, default=100)
    args = parser.parse_args()
    records = load_trace(args.input)
    sources = resolve_sources(records, args.elf)
    enrich_trace(records, sources, args.trace_out)
    args.report_out.write_text(analyze(records, sources, args.poll_threshold), encoding="utf-8")
    args.profile_out.write_text(json.dumps(make_profile(records), indent=2) + "\n",
                                encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
