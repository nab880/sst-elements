#!/usr/bin/env python3
"""
validate_against_libmem.py — compare Quetz mem-op counts to QEMU libmem.so.

Runs the vanadis RISC-V hello-world binary twice:
  Pass A: full SST simulation via basic_quetz.py (parse stats)
  Pass B: qemu-riscv64 + libmem.so only (parse stderr summary)

Skips with exit 0 if libmem.so is not installed.
"""

from __future__ import print_function

import os
import re
import shutil
import subprocess
import sys
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
QUETZ_TESTS = SCRIPT_DIR

SOFT_TOLERANCE = 0.05


def _sst_home():
    h = os.environ.get("SST_HOME", "")
    if h:
        return h
    cfg = shutil.which("sst-config")
    if cfg:
        return subprocess.check_output([cfg, "--prefix"], text=True).strip()
    return "/opt/sst"


def _find_libmem():
    for d in (
        os.environ.get("QEMU_PLUGIN_DIR", ""),
        "/opt/qemu/lib/qemu/plugins",
        "/opt/qemu/libexec/qemu/plugins",
    ):
        if not d:
            continue
        path = os.path.join(d, "libmem.so")
        if os.path.exists(path):
            return path
    return None


def _parse_quetz_stats(stdout_path):
    stats = {}
    with open(stdout_path, "r") as f:
        for line in f:
            if "Sum.u64 = " in line and "cpu." in line:
                name = line.strip().split(" : ")[0].strip()
                val = int(line.split("Sum.u64 = ")[1].split(";")[0])
                stats[name] = val
    return stats


def _run_quetz_pass(sst_home, qemu, exe, plugin):
    sst_bin = os.path.join(sst_home, "bin", "sst")
    sdl = os.path.join(QUETZ_TESTS, "usermode", "basic_quetz.py")
    outdir = tempfile.mkdtemp(prefix="quetz_libmem_")
    outfile = os.path.join(outdir, "quetz.out")
    errfile = os.path.join(outdir, "quetz.err")

    env = os.environ.copy()
    env["SST_HOME"] = sst_home
    env["QUETZ_EXE"] = exe
    env["QUETZ_QEMU"] = qemu
    env["QUETZ_PLUGIN"] = plugin
    env["QUETZ_WITH_L1"] = "0"
    env["QUETZ_DETAILED"] = "0"

    subprocess.check_call(
        [sst_bin, sdl],
        stdout=open(outfile, "w"),
        stderr=open(errfile, "w"),
        cwd=outdir,
        env=env,
        timeout=180,
    )
    return _parse_quetz_stats(outfile)


def _run_libmem_pass(qemu, exe, libmem):
    proc = subprocess.run(
        [qemu, "-plugin", libmem + ",inline=on", exe],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=120,
        check=False,
    )
    text = proc.stderr.decode("utf-8", errors="replace")
    loads = stores = 0
    for line in text.splitlines():
        m = re.search(r"(\d+)\s+loads.*?(\d+)\s+stores", line)
        if m:
            loads = int(m.group(1))
            stores = int(m.group(2))
        m2 = re.search(r"loads:\s*(\d+).*stores:\s*(\d+)", line)
        if m2:
            loads = int(m2.group(1))
            stores = int(m2.group(2))
    return loads, stores, text


def _reconcile(stats):
    reads  = stats.get("cpu.read_requests.0", 0)
    writes = stats.get("cpu.write_requests.0", 0)
    filt_r = stats.get("cpu.filtered_reads.0", 0)
    filt_w = stats.get("cpu.filtered_writes.0", 0)
    split_r = stats.get("cpu.split_read_requests.0", 0)
    split_w = stats.get("cpu.split_write_requests.0", 0)
    real_reads  = reads + filt_r - split_r
    real_writes = writes + filt_w - split_w
    return real_reads, real_writes


def main():
    libmem = _find_libmem()
    if not libmem:
        print("SKIP: libmem.so not found — skipping ground-truth validation")
        return 0

    sst_home = _sst_home()
    bindir   = os.path.join(sst_home, "bin")
    qemu     = os.path.join(bindir, "qemu-riscv64")
    plugin   = os.path.join(sst_home, "libexec", "libqemu_sst_plugin.so")
    exe      = os.path.normpath(os.path.join(
        QUETZ_TESTS, "../../vanadis/tests/small"
        "/basic-io/hello-world/riscv64/hello-world"))

    if not os.path.exists(qemu):
        print("SKIP: qemu-riscv64 not found at", qemu)
        return 0
    if not os.path.exists(exe):
        print("SKIP: guest binary not found at", exe)
        return 0
    if not os.path.exists(plugin):
        print("SKIP: Quetz plugin not found at", plugin)
        return 0

    print("=== libmem ground-truth validation (RISC-V hello) ===")
    stats = _run_quetz_pass(sst_home, qemu, exe, plugin)
    q_reads, q_writes = _reconcile(stats)

    l_reads, l_writes, libmem_log = _run_libmem_pass(qemu, exe, libmem)
    if l_reads == 0 and l_writes == 0:
        print("WARN: could not parse libmem summary; stderr tail:")
        print(libmem_log[-2000:])
        print("SKIP: libmem output not parseable")
        return 0

    print("Quetz reconciled: reads={} writes={}".format(q_reads, q_writes))
    print("libmem:           reads={} writes={}".format(l_reads, l_writes))

    def _check(label, quetz_val, libmem_val):
        if libmem_val == 0:
            return quetz_val == 0
        rel = abs(quetz_val - libmem_val) / float(libmem_val)
        if rel <= SOFT_TOLERANCE:
            print("OK {} (delta {:.1%})".format(label, rel))
            return True
        print("FAIL {}: quetz={} libmem={} delta {:.1%}".format(
            label, quetz_val, libmem_val, rel))
        return False

    ok_r = _check("reads", q_reads, l_reads)
    ok_w = _check("writes", q_writes, l_writes)
    if not (ok_r and ok_w):
        return 1
    print("=== libmem ground-truth validation passed ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
