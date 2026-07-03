#!/usr/bin/env python3
"""dump_stream.py — inspect/verify a QuetzStreamDevice or sink fixture.

Prints length, a hex preview, and — unless --no-trailer — verifies the
fixture's last 4 bytes against the little-endian sum32 of the payload
(the make_stream.py convention). Exit 0 if the trailer checks out (or
--no-trailer), 1 otherwise.

  dump_stream.py sensor.bin
  dump_stream.py --s16 sensor.bin      # also decode payload as LE int16
  dump_stream.py --no-trailer cap.bin
"""

import argparse
import struct
import sys


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("--no-trailer", action="store_true",
                    help="treat the whole file as payload (no sum32 check)")
    ap.add_argument("--s16", action="store_true",
                    help="decode the payload as little-endian int16 samples")
    ap.add_argument("--head", type=int, default=32,
                    help="bytes of hex preview (default 32)")
    args = ap.parse_args()

    with open(args.input, "rb") as f:
        data = f.read()

    if args.no_trailer:
        payload, ok = data, True
        print("{}: {} bytes (no trailer)".format(args.input, len(data)))
    else:
        if len(data) < 5:
            sys.exit("dump_stream: too short for payload + sum32 trailer")
        payload = data[:-4]
        expect = struct.unpack("<I", data[-4:])[0]
        actual = sum(payload) & 0xFFFFFFFF
        ok = expect == actual
        print("{}: {} payload bytes + trailer; sum32 {}=0x{:08x} {}".format(
            args.input, len(payload),
            "expect" if ok else "EXPECT", expect,
            "OK" if ok else "!= actual 0x{:08x}".format(actual)))

    preview = payload[:args.head]
    print("head: " + " ".join("{:02x}".format(b) for b in preview)
          + (" ..." if len(payload) > len(preview) else ""))
    if args.s16:
        n = len(payload) // 2
        samples = struct.unpack("<{}h".format(n), payload[:n * 2])
        print("s16[{}]: {}{}".format(
            n, ", ".join(str(s) for s in samples[:16]),
            " ..." if n > 16 else ""))

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
