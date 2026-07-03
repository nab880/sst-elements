#!/usr/bin/env python3
"""check_nmea.py — validate (or repair) NMEA checksums in a GPS log.

Turns a real GPS capture into a replay fixture for the quetz decks (fed into
UART RX via QUETZ_STDIN_FILE / quetz-run --stdin). Sentences are
"$<body>*HH" where HH is the XOR of the body bytes.

  check_nmea.py capture.nmea                # report; exit 1 if any invalid
  check_nmea.py --fix capture.nmea out.txt  # rewrite with correct checksums
  check_nmea.py --only GPRMC in.txt out.txt # also filter by sentence type

Lines without '$' or '*' are reported as malformed (kept as-is with --fix,
so deliberately-corrupted test fixtures survive a report pass).
"""

import argparse
import functools
import sys


def checksum(body):
    return functools.reduce(lambda a, c: a ^ ord(c), body, 0)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("output", nargs="?",
                    help="required with --fix / --only")
    ap.add_argument("--fix", action="store_true",
                    help="rewrite every well-formed sentence with the correct checksum")
    ap.add_argument("--only", metavar="TYPE",
                    help="keep only $<TYPE> sentences (e.g. GPRMC)")
    args = ap.parse_args()

    if (args.fix or args.only) and not args.output:
        sys.exit("check_nmea: --fix/--only need an output file")

    with open(args.input, "r") as f:
        lines = f.read().splitlines()

    out_lines, valid, invalid, malformed = [], 0, 0, 0
    for i, line in enumerate(lines, 1):
        s = line.strip()
        if not s:
            continue
        if not s.startswith("$") or "*" not in s:
            malformed += 1
            print("line {}: malformed (no $/*): {}".format(i, s))
            out_lines.append(s)
            continue
        body, _, ck = s[1:].rpartition("*")
        if args.only and not body.startswith(args.only):
            continue
        want = checksum(body)
        try:
            have = int(ck, 16)
        except ValueError:
            have = -1
        if have == want:
            valid += 1
            out_lines.append(s)
        else:
            invalid += 1
            print("line {}: bad checksum *{} (want *{:02X}): {}".format(
                i, ck, want, s))
            out_lines.append("${}*{:02X}".format(body, want) if args.fix else s)

    print("{}: {} valid, {} invalid, {} malformed".format(
        args.input, valid, invalid, malformed))
    if args.output:
        with open(args.output, "w") as f:
            f.write("\n".join(out_lines) + "\n")
        print("wrote {} ({} sentences)".format(args.output, len(out_lines)))

    sys.exit(0 if (invalid == 0 and malformed == 0) or args.fix else 1)


if __name__ == "__main__":
    main()
