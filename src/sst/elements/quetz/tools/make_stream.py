#!/usr/bin/env python3
"""make_stream.py — turn recorded data into a QuetzStreamDevice fixture.

The stream device replays a binary file to the guest; by convention (see the
coldfire_system demo) the last 4 bytes are a little-endian sum32 of the
payload so firmware can verify integrity. This tool builds such fixtures from
common recording formats:

  make_stream.py samples.csv sensor.bin            # CSV of ints -> LE s16
  make_stream.py --u8 samples.csv sensor.bin       # CSV of ints -> u8
  make_stream.py --raw capture.bin sensor.bin      # bytes passthrough
  make_stream.py --raw --no-trailer c.bin s.bin    # no sum32 trailer

CSV input: integers separated by commas and/or newlines ('#' comments ok).
The inverse (inspect/verify a fixture) is dump_stream.py.
"""

import argparse
import struct
import sys


def parse_csv_ints(text):
    values = []
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        for tok in line.replace(",", " ").split():
            values.append(int(tok, 0))
    return values


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="CSV of integers, or raw binary with --raw")
    ap.add_argument("output", help="fixture .bin to write")
    fmt = ap.add_mutually_exclusive_group()
    fmt.add_argument("--raw", action="store_true",
                     help="input is raw bytes (passthrough)")
    fmt.add_argument("--u8", action="store_true",
                     help="CSV values are unsigned bytes (default: LE int16)")
    ap.add_argument("--no-trailer", action="store_true",
                    help="do not append the sum32 trailer")
    args = ap.parse_args()

    if args.raw:
        with open(args.input, "rb") as f:
            payload = f.read()
    else:
        with open(args.input, "r") as f:
            values = parse_csv_ints(f.read())
        if not values:
            sys.exit("make_stream: no values parsed from {}".format(args.input))
        try:
            if args.u8:
                payload = bytes((v & 0xFF) for v in values)
            else:
                payload = b"".join(struct.pack("<h", v) for v in values)
        except struct.error as e:
            sys.exit("make_stream: value out of int16 range: {}".format(e))

    if not payload:
        sys.exit("make_stream: empty payload")

    out = payload if args.no_trailer else \
        payload + struct.pack("<I", sum(payload) & 0xFFFFFFFF)
    with open(args.output, "wb") as f:
        f.write(out)
    print("{}: {} payload bytes{}, {} total".format(
        args.output, len(payload),
        "" if args.no_trailer else " + sum32 trailer", len(out)))


if __name__ == "__main__":
    main()
