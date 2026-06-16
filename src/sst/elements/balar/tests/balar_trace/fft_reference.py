#!/usr/bin/env python3
# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S.
# Government retains certain rights in this software.
#
# Copyright (c) 2009-2026, NTESS
# All rights reserved.
#
# fft_reference.py — host-only twin of fft.cu's staged radix-2 FFT.
#
# Purpose:
#   1. Validate the kernel ALGORITHM (bit-reversal, stage/twiddle indexing)
#      independently of CUDA so bugs are caught on the host, not in GPGPU-Sim.
#   2. Prove the exact test vectors are bit-exact in float32:
#        impulse x[n]=delta[n] -> X[k] = 1+0j for all k
#        DC      x[n]=1         -> X[0]=N, rest 0
#      (twiddles only ever multiply zero in these cases, so no rounding.)
#   3. Emit the little-endian float32 .data files the trace tests consume:
#        input vector (H2D), twiddles (H2D), expected output (D2H gold).
#
# No numpy dependency (the demo host may not have it).

import argparse
import cmath
import math
import os
import struct

# Itanium-mangled device names emitted by nvcc for the fft.cu kernels.
# Calibrated against the known balar example (vecAdd -> _Z6vecAddPiS_S_i):
# builtins (int) are not substitution candidates; struct float2 (6float2) is S_.
#   fft_bitrev(float2*, const float2*, int, int)        -> P6float2 PKS_ i i
#   fft_stage (float2*, const float2*, int, int, int)   -> P6float2 PKS_ i i i
# Confirm after building with:  cuobjdump -sass balar_trace/fft | grep -i fft_
PTX_BITREV = "_Z10fft_bitrevP6float2PKS_ii"
PTX_STAGE = "_Z9fft_stageP6float2PKS_iii"
PTX_SHARED = "_Z10fft_sharedP6float2PKS_ii"


def bitrev(x, logn):
    r = 0
    for _ in range(logn):
        r = (r << 1) | (x & 1)
        x >>= 1
    return r


def twiddles(n):
    # W_N^t = exp(-2*pi*i*t/N), t = 0 .. N/2-1
    return [cmath.exp(-2j * math.pi * t / n) for t in range(n // 2)]


def fft_staged(x, tw, n, logn):
    """Mirror of fft.cu: bit-reversal then log2(N) in-place butterfly stages."""
    a = [x[bitrev(i, logn)] for i in range(n)]
    for s in range(1, logn + 1):
        half = 1 << (s - 1)
        for k in range(n // 2):
            j = k & (half - 1)
            group = k >> (s - 1)
            i0 = group * (half << 1) + j
            i1 = i0 + half
            w = tw[j << (logn - s)]
            t = w * a[i1]
            a[i0], a[i1] = a[i0] + t, a[i0] - t
    return a


def naive_dft(x, n):
    return [sum(x[m] * cmath.exp(-2j * math.pi * k * m / n) for m in range(n))
            for k in range(n)]


def f32(v):
    """Round a Python float to float32 (round-trip through 4-byte IEEE-754)."""
    return struct.unpack("<f", struct.pack("<f", v))[0]


def write_complex_f32(path, data):
    with open(path, "wb") as fh:
        for c in data:
            fh.write(struct.pack("<f", c.real))
            fh.write(struct.pack("<f", c.imag))


def read_complex_f32(path):
    raw = open(path, "rb").read()
    vals = struct.unpack("<%df" % (len(raw) // 4), raw)
    return [complex(vals[i], vals[i + 1]) for i in range(0, len(vals), 2)]


def compare_dump(dump_path, kind, n):
    """Compare a GPU D2H dump (float32 complex) against an independent naive-DFT
    reference for `kind`, with a relative tolerance. Returns (ok, info)."""
    got = read_complex_f32(dump_path)
    if len(got) != n:
        return False, "dump has %d points, expected %d" % (len(got), n)
    ref = naive_dft(vector(kind, n), n)
    scale = max(abs(v) for v in ref) or 1.0
    l2_err = math.sqrt(sum(abs(got[k] - ref[k]) ** 2 for k in range(n)))
    l2_ref = math.sqrt(sum(abs(ref[k]) ** 2 for k in range(n))) or 1.0
    rel_l2 = l2_err / l2_ref
    max_err = max(abs(got[k] - ref[k]) for k in range(n)) / scale
    peak_got = max(range(n), key=lambda k: abs(got[k]))
    # A real cosine has two equal peaks (k0 and N-k0); which one wins is decided
    # by rounding, so accept either. rel-L2 already validates the full spectrum.
    expected = (TONE_BIN, n - TONE_BIN) if kind == "tone" else None
    peak_ok = (peak_got in expected) if expected else True
    ok = rel_l2 < 1e-3 and peak_ok
    info = ("rel-L2={:.2e} max={:.2e} gpu peak bin={} (expected {})"
            .format(rel_l2, max_err, peak_got,
                    "/".join(map(str, expected)) if expected else "n/a"))
    return ok, info


# Spectral bin of the "tone" test vector: a real cosine x[n]=cos(2*pi*k0*n/N)
# whose FFT is a clean pair of peaks at k0 and N-k0. Unlike impulse/DC, the tone
# drives every twiddle non-trivially, so it actually validates the GPU's
# complex-multiply / twiddle math (which impulse cannot).
TONE_BIN = 5


def vector(kind, n):
    if kind == "impulse":
        return [complex(1.0 if i == 0 else 0.0, 0.0) for i in range(n)]
    if kind == "dc":
        return [complex(1.0, 0.0) for i in range(n)]
    if kind == "tone":
        return [complex(f32(math.cos(2.0 * math.pi * TONE_BIN * i / n)), 0.0)
                for i in range(n)]
    if kind == "ramp":  # perf-only vector; not bit-exact vs a CPU FFT
        return [complex(f32((i % 17) - 8), f32((i % 5) - 2)) for i in range(n)]
    raise ValueError(kind)


def reference_output(kind, n, tw, logn):
    """Float32 reference FFT output for a vector kind (gold for the D2H file)."""
    exact = expected_exact(kind, n)
    if exact is not None:
        return exact
    return fft_staged(vector(kind, n), tw, n, logn)


def expected_exact(kind, n):
    if kind == "impulse":
        return [complex(1.0, 0.0) for _ in range(n)]
    if kind == "dc":
        return [complex(float(n) if k == 0 else 0.0, 0.0) for k in range(n)]
    return None


def _launch_line(name, ptx, grid, block, shared, args):
    return ("CUDA cu kernel launch detected: name: {nm}, ptx_name: {ptx}, "
            "funcptr: 0x0, gdx: {g}, gdy: 1, gdz: 1, bdx: {b}, bdy: 1, "
            "bdz: 1, sharedBytes: {sh}, CUstream: (nil), args: {args}").format(
                nm=name, ptx=ptx, g=grid, b=block, sh=shared, args=args)


def write_trace(path, kind, n, logn, mode="staged", block=256):
    """Emit an FFT CUDA API trace replayable by Quetz/BalarTestCPU.

    mode="staged": dptr-0=input, dptr-1=work/output, dptr-2=twiddles; 3x malloc,
      H2D(input), H2D(twiddles), bit-reversal launch, log2(N) butterfly-stage
      launches (global memory), D2H(output gold), 3x free.
    mode="shared": dptr-0=in/out, dptr-1=twiddles; 2x malloc, 2x H2D, a single
      fft_shared launch (all stages on-chip in N*8 bytes of shared memory),
      D2H, 2x free.
    """
    vec_bytes = n * 8          # N complex float32
    tw_bytes = (n // 2) * 8

    if mode == "shared":
        threads = min(n // 2, 1024)
        lines = [
            "CUDA memalloc: dptr: dptr-0, size: %d" % vec_bytes,
            "CUDA memalloc: dptr: dptr-1, size: %d" % tw_bytes,
            "CUDA memcpyH2D detected: device_ptr: dptr-0, host_ptr: 0x0, size: %d, "
            "data_file: fft_%s_in_%d.data, addr: 0x0" % (vec_bytes, kind, n),
            "CUDA memcpyH2D detected: device_ptr: dptr-1, host_ptr: 0x0, size: %d, "
            "data_file: fft_tw_%d.data, addr: 0x0" % (tw_bytes, n // 2),
            _launch_line("fft_shared", PTX_SHARED, 1, threads, vec_bytes,
                         "dptr-0/8/dptr-1/8/%d/4/%d/4/" % (n, logn)),
            "CUDA memcpyD2H detected: host_ptr: 0x0, device_ptr: dptr-0, size: %d, "
            "data_file: fft_%s_out_%d.data" % (vec_bytes, kind, n),
            "CUDA free detected: dptr: dptr-0",
            "CUDA free detected: dptr: dptr-1",
        ]
        with open(path, "w") as fh:
            fh.write("\n".join(lines) + "\n")
        return 1 + 2 + 2 + 1 + 1 + 2   # fatbin + 2 malloc + 2 H2D + launch + D2H + 2 free

    bitrev_grid = (n + block - 1) // block
    stage_grid = ((n // 2) + block - 1) // block
    lines = [
        "CUDA memalloc: dptr: dptr-0, size: %d" % vec_bytes,
        "CUDA memalloc: dptr: dptr-1, size: %d" % vec_bytes,
        "CUDA memalloc: dptr: dptr-2, size: %d" % tw_bytes,
        "CUDA memcpyH2D detected: device_ptr: dptr-0, host_ptr: 0x0, size: %d, "
        "data_file: fft_%s_in_%d.data, addr: 0x0" % (vec_bytes, kind, n),
        "CUDA memcpyH2D detected: device_ptr: dptr-2, host_ptr: 0x0, size: %d, "
        "data_file: fft_tw_%d.data, addr: 0x0" % (tw_bytes, n // 2),
        _launch_line("fft_bitrev", PTX_BITREV, bitrev_grid, block, 0,
                     "dptr-1/8/dptr-0/8/%d/4/%d/4/" % (n, logn)),
    ]
    for s in range(1, logn + 1):
        lines.append(_launch_line("fft_stage", PTX_STAGE, stage_grid, block, 0,
                                  "dptr-1/8/dptr-2/8/%d/4/%d/4/%d/4/" % (n, s, logn)))
    lines += [
        "CUDA memcpyD2H detected: host_ptr: 0x0, device_ptr: dptr-1, size: %d, "
        "data_file: fft_%s_out_%d.data" % (vec_bytes, kind, n),
        "CUDA free detected: dptr: dptr-0",
        "CUDA free detected: dptr: dptr-1",
        "CUDA free detected: dptr: dptr-2",
    ]
    with open(path, "w") as fh:
        fh.write("\n".join(lines) + "\n")
    # fatbin + 3 malloc + 2 H2D + (reg_function inline) + (1 + logn) launches + D2H + 3 free.
    return 1 + 3 + 2 + (1 + logn) + 1 + 3


def write_firmware_header(path, n, logn):
    """Emit a C header with the twiddle table for the bare-metal sysmode guest.

    The firmware has no libm, so twiddles are precomputed here as IEEE-754
    float32 bit patterns interleaved (re, im) and reinterpreted as float2[N/2].
    """
    tw = twiddles(n)
    words = []
    for c in tw:
        words.append(struct.unpack("<I", struct.pack("<f", f32(c.real)))[0])
        words.append(struct.unpack("<I", struct.pack("<f", f32(c.imag)))[0])
    guard = "FFT_FIRMWARE_DATA_%d_H" % n
    lines = [
        "/* Auto-generated by fft_reference.py --header. Do not edit. */",
        "#ifndef %s" % guard,
        "#define %s" % guard,
        "#define FFT_N %d" % n,
        "#define FFT_LOGN %d" % logn,
        "/* W_N^t = exp(-2*pi*i*t/N), t=0..N/2-1, float32 bits, interleaved",
        "   (re,im); reinterpret as float2[N/2]. */",
        "static const unsigned int fft_tw_bits[FFT_N] = {",
    ]
    for i in range(0, len(words), 8):
        lines.append("    " + ", ".join("0x%08x" % w for w in words[i:i + 8]) + ",")
    lines += ["};", "#endif /* %s */" % guard]
    with open(path, "w") as fh:
        fh.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser(description="FFT reference + .data generator")
    ap.add_argument("-n", type=int, default=256, help="FFT size (power of two)")
    ap.add_argument("--outdir", default=".", help="where to write .data files")
    ap.add_argument("--emit", action="store_true", help="write .data files")
    ap.add_argument("--data-only", action="store_true",
                    help="with --emit, write only .data files (not .trace); "
                         "the .trace is committed, the .data is generated at "
                         "test setup (it is .gitignored)")
    ap.add_argument("--header", metavar="PATH",
                    help="write a firmware twiddle C header to PATH")
    ap.add_argument("--compare", metavar="DUMP",
                    help="compare a GPU D2H dump (float32) to the DFT reference")
    ap.add_argument("--kind", default="tone",
                    help="vector kind for --compare (default: tone)")
    args = ap.parse_args()

    n = args.n
    logn = int(round(math.log2(n)))
    assert (1 << logn) == n, "N must be a power of two"
    tw = twiddles(n)

    if args.compare:
        ok, info = compare_dump(args.compare, args.kind, n)
        print("[compare] kind=%s N=%d : %s" % (args.kind, n, info))
        print("RESULT:", "PASS" if ok else "FAIL")
        return 0 if ok else 1

    ok = True
    for kind in ("impulse", "dc"):
        x = vector(kind, n)
        got = fft_staged(x, tw, n, logn)
        exp = expected_exact(kind, n)
        # Bit-exact in float32: every value must round-trip identically.
        exact = all(f32(g.real) == f32(e.real) and f32(g.imag) == f32(e.imag)
                    for g, e in zip(got, exp))
        ok = ok and exact
        print("[{:7s}] N={:<5d} staged==exact: {}".format(kind, n, exact))

    # The tone is the real correctness vector: it drives every twiddle, so this
    # confirms the staged FFT matches an independent DFT (impulse cannot).
    for kind in ("tone", "ramp"):
        a = fft_staged(vector(kind, n), tw, n, logn)
        d = naive_dft(vector(kind, n), n)
        rel = max(abs(a[k] - d[k]) for k in range(n)) / (max(abs(v) for v in d) or 1.0)
        print("[{:7s}] N={:<5d} staged-vs-DFT max rel err: {:.3e}".format(kind, n, rel))
        ok = ok and rel < 1e-4

    if args.emit:
        os.makedirs(args.outdir, exist_ok=True)
        write_complex_f32(os.path.join(args.outdir, "fft_tw_%d.data" % (n // 2)), tw)
        for kind in ("impulse", "dc", "tone"):
            write_complex_f32(
                os.path.join(args.outdir, "fft_%s_in_%d.data" % (kind, n)),
                vector(kind, n))
            write_complex_f32(
                os.path.join(args.outdir, "fft_%s_out_%d.data" % (kind, n)),
                reference_output(kind, n, tw, logn))
            if not args.data_only:
                rt = write_trace(
                    os.path.join(args.outdir, "fft_%s_%d.trace" % (kind, n)),
                    kind, n, logn, mode="staged")
                rs = write_trace(
                    os.path.join(args.outdir, "fft_%s_%d_shared.trace" % (kind, n)),
                    kind, n, logn, mode="shared")
                print("  fft_%s_%d.trace : %d round-trips (staged); "
                      "_shared : %d (shared)" % (kind, n, rt, rs))
        print("emitted %s files to %s" % (
            ".data" if args.data_only else ".data + .trace",
            os.path.abspath(args.outdir)))

    if args.header:
        write_firmware_header(args.header, n, logn)
        print("wrote firmware twiddle header:", os.path.abspath(args.header))

    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
