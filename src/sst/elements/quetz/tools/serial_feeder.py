#!/usr/bin/env python3
"""serial_feeder.py — feed a recorded file into a QEMU `-serial pipe:` port.

QEMU's pipe chardev reads <path>.in and writes <path>.out (both must exist as
FIFOs before QEMU starts; QEMU opens them O_RDWR, so this writer can exit
after the data is queued without the guest seeing EOF). Line pacing makes the
replay behave like the real device — a GPS emits ~1 sentence per second, and
burst replay masks flow-control and polling bugs.

  serial_feeder.py gps.nmea /tmp/feed/gps.in --hz 5

The harness wrapper is make_serial_feed() in tests/quetz_test_helpers.py;
quetz-run users: create the FIFOs, start this, add `-serial pipe:<base>` to
QUETZ_QEMU_ARGS (see SIMULATING-YOUR-SYSTEM.md).
"""

import argparse
import sys
import time


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("data_file", help="file to replay")
    ap.add_argument("fifo", help="the chardev's <path>.in FIFO")
    ap.add_argument("--hz", type=float, default=0.0,
                    help="lines per second (0 = write everything immediately)")
    args = ap.parse_args()

    with open(args.data_file, "rb") as f:
        data = f.read()

    # O_RDWR ('r+b') so the open never blocks waiting for a reader and the
    # FIFO holds queued bytes even if QEMU opens it later.
    out = open(args.fifo, "r+b", buffering=0)

    if args.hz <= 0:
        out.write(data)
        return

    period = 1.0 / args.hz
    for line in data.splitlines(keepends=True):
        out.write(line)
        time.sleep(period)


if __name__ == "__main__":
    sys.exit(main())
