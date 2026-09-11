#!/usr/bin/env python3
"""Verify ColdFire cache instructions and register callbacks in the actual QEMU.

Requires matching QEMU plugin headers, GLib development files, and an m68k
cross compiler. Runs tiny public firmware only; no BSP sources or SST needed.
"""
import argparse
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", default="qemu-system-m68k")
    parser.add_argument("--cc", default="m68k-linux-gnu-gcc")
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--qemu-plugin-include", default="/opt/qemu/include")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    print(subprocess.check_output([args.qemu, "--version"], text=True).splitlines()[0])
    expected = [
        "OP kind=0 control=002 source=0 value=01000000 instruction=4e7b",
        "OP kind=0 control=004 source=1 value=1000c020 instruction=4e7b",
        "OP kind=0 control=005 source=7 value=2000c040 instruction=4e7b",
        "OP kind=0 control=006 source=8 value=3000c000 instruction=4e7b",
        "OP kind=0 control=007 source=14 value=4000c000 instruction=4e7b",
        "OP kind=1 control=000 source=8 value=00000000 instruction=f468",
        "OP kind=1 control=000 source=8 value=00000000 instruction=f4a8",
        "OP kind=1 control=000 source=8 value=00000000 instruction=f4e8",
    ]
    with tempfile.TemporaryDirectory(prefix="quetz-cache-api-") as tmp:
        tmp = Path(tmp)
        plugin = tmp / "probe.so"
        firmware = tmp / "probe.elf"
        glib = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "glib-2.0"], text=True))
        linker = ["-undefined", "dynamic_lookup"] if sys.platform == "darwin" else []
        subprocess.run([args.cxx, "-std=c++17", "-shared", "-fPIC", *linker,
                        "-I" + args.qemu_plugin_include,
                        "-I" + str(here.parent.parent / "qemu_plugin"), *glib,
                        str(here / "coldfire_cache_api_probe.cpp"), "-o", str(plugin)],
                       check=True)
        subprocess.run([args.cc, "-mcpu=5475", "-nostdlib", "-nostartfiles",
                        "-Wl,-Ttext=0x40000000", "-Wl,-e,_start",
                        str(here / "coldfire_cache_api_probe.S"), "-o", str(firmware)],
                       check=True)
        for cpu in ("m5208", "cfv4e"):
            command = [args.qemu, "-M", "mcf5208evb", "-cpu", cpu,
                       "-display", "none", "-monitor", "none", "-serial", "stdio",
                       "-kernel", str(firmware), "-plugin", str(plugin)]
            # Successful firmware spins after its UART marker. A short timeout
            # terminates that idle loop; unexpected early exit is a failure.
            try:
                result = subprocess.run(command, capture_output=True, timeout=2)
            except subprocess.TimeoutExpired as exc:
                stdout = (exc.stdout or b"").decode(errors="replace")
                stderr = (exc.stderr or b"").decode(errors="replace")
            else:
                raise AssertionError(f"{cpu}: QEMU exited {result.returncode}: "
                                     + result.stderr.decode(errors="replace"))
            observed = [line for line in stderr.splitlines() if line.startswith("OP ")]
            if observed != expected or "OK\n" not in stdout:
                raise AssertionError(f"{cpu}: cache probe mismatch\n{stdout}\n{stderr}")
            print(f"PASS {cpu}: 5 MOVEC operands, 3 CPUSHL selectors, UART completion")


if __name__ == "__main__":
    main()
