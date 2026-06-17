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
# builtins (int/float/double) are not substitution candidates; the vector struct
# (6float2 or 7double2) is S_. fft_scale's last arg is the real scalar (f or d).
# Confirm after building with:  cuobjdump -ptx balar_trace/fft | grep entry
def ptx_names(double=False):
    v = "7double2" if double else "6float2"
    sc = "d" if double else "f"
    return {
        "bitrev": "_Z10fft_bitrevP%sPKS_ii" % v,
        "stage":  "_Z9fft_stageP%sPKS_iii" % v,
        "shared": "_Z10fft_sharedP%sPKS_ii" % v,
        "scale":  "_Z9fft_scaleP%si%s" % (v, sc),
    }


class Prec:
    """Precision config: float2 (default) or double2 (--double)."""
    def __init__(self, double=False):
        self.double = double
        self.fmt = "d" if double else "f"     # struct format, per component
        self.comp = 8 if double else 4        # bytes per component
        self.cplx = 16 if double else 8       # bytes per complex point
        self.ptx = ptx_names(double)
        self.name = "double" if double else "float"


# Module-level active precision (set from --double in main()).
PREC = Prec(False)


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


def write_complex(path, data):
    """Write little-endian complex samples in the active precision (f32 or f64)."""
    fmt = "<" + PREC.fmt
    with open(path, "wb") as fh:
        for c in data:
            fh.write(struct.pack(fmt, c.real))
            fh.write(struct.pack(fmt, c.imag))


def read_complex(path):
    raw = open(path, "rb").read()
    cnt = len(raw) // PREC.comp
    vals = struct.unpack("<%d%s" % (cnt, PREC.fmt), raw)
    return [complex(vals[i], vals[i + 1]) for i in range(0, len(vals), 2)]


def conj_twiddles(n):
    return [t.conjugate() for t in twiddles(n)]


def ifft_staged(X, twc, n, logn):
    """Inverse FFT: the same staged kernels with conjugated twiddles, /N."""
    a = fft_staged(X, twc, n, logn)
    return [v / n for v in a]


def fcast(v):
    """Round a real to the active precision (float32 narrowing, identity f64)."""
    return float(v) if PREC.double else f32(v)


def compare_dump(dump_path, kind, n, roundtrip=False):
    """Compare a GPU D2H dump against a host reference, with a relative tolerance.

    roundtrip=False: forward FFT vs an independent naive DFT (+ peak-bin sanity).
    roundtrip=True : IFFT(FFT(x)) vs the original input x (self-consistency).
    Returns (ok, info)."""
    got = read_complex(dump_path)
    if len(got) != n:
        return False, "dump has %d points, expected %d" % (len(got), n)
    if roundtrip:
        ref = vector(kind, n)                 # recovered x must match the input
        label = "roundtrip"
    else:
        ref = naive_dft(vector(kind, n), n)   # spectrum vs independent DFT
        label = "fwd"
    scale = max(abs(v) for v in ref) or 1.0
    l2_err = math.sqrt(sum(abs(got[k] - ref[k]) ** 2 for k in range(n)))
    l2_ref = math.sqrt(sum(abs(ref[k]) ** 2 for k in range(n))) or 1.0
    rel_l2 = l2_err / l2_ref
    max_err = max(abs(got[k] - ref[k]) for k in range(n)) / scale
    peak_info = ""
    peak_ok = True
    if not roundtrip:
        # A real cosine has two equal peaks (k0 and N-k0); which one wins is
        # decided by rounding, so accept either. rel-L2 validates the spectrum.
        peak_got = max(range(n), key=lambda k: abs(got[k]))
        expected = (TONE_BIN, n - TONE_BIN) if kind == "tone" else None
        peak_ok = (peak_got in expected) if expected else True
        peak_info = " peak bin={} (expected {})".format(
            peak_got, "/".join(map(str, expected)) if expected else "n/a")
    ok = rel_l2 < 1e-3 and peak_ok
    info = "{} prec={} rel-L2={:.2e} max={:.2e}{}".format(
        label, PREC.name, rel_l2, max_err, peak_info)
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
        return [complex(fcast(math.cos(2.0 * math.pi * TONE_BIN * i / n)), 0.0)
                for i in range(n)]
    if kind == "ramp":  # perf-only vector; not bit-exact vs a CPU FFT
        return [complex(fcast((i % 17) - 8), fcast((i % 5) - 2)) for i in range(n)]
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


def _malloc(dptr, sz):
    return "CUDA memalloc: dptr: %s, size: %d" % (dptr, sz)


def _h2d(dptr, sz, fn):
    return ("CUDA memcpyH2D detected: device_ptr: %s, host_ptr: 0x0, size: %d, "
            "data_file: %s, addr: 0x0" % (dptr, sz, fn))


def _d2h(dptr, sz, fn):
    return ("CUDA memcpyD2H detected: host_ptr: 0x0, device_ptr: %s, size: %d, "
            "data_file: %s" % (dptr, sz, fn))


def write_trace(path, kind, n, logn, mode="staged", block=256):
    """Emit an FFT CUDA API trace replayable by Quetz/BalarTestCPU.

    mode="staged"   : bit-reversal + log2(N) butterfly stages in global memory.
    mode="shared"   : a single on-chip fft_shared launch.
    mode="roundtrip": forward FFT then inverse FFT (conjugated twiddles + 1/N
                      scale); the D2H gold is the original input, so the recovered
                      signal is tolerance-checked with --compare --roundtrip.
    Buffer/element sizes and kernel names follow the active precision (PREC).
    """
    cb = PREC.cplx                 # bytes per complex point (8 f32 / 16 f64)
    vec_bytes = n * cb
    tw_bytes = (n // 2) * cb
    p = PREC.ptx
    bitrev_grid = (n + block - 1) // block
    stage_grid = ((n // 2) + block - 1) // block

    if mode == "shared":
        threads = min(n // 2, 1024)
        lines = [
            _malloc("dptr-0", vec_bytes), _malloc("dptr-1", tw_bytes),
            _h2d("dptr-0", vec_bytes, "fft_%s_in_%d.data" % (kind, n)),
            _h2d("dptr-1", tw_bytes, "fft_tw_%d.data" % (n // 2)),
            _launch_line("fft_shared", p["shared"], 1, threads, vec_bytes,
                         "dptr-0/8/dptr-1/8/%d/4/%d/4/" % (n, logn)),
            _d2h("dptr-0", vec_bytes, "fft_%s_out_%d.data" % (kind, n)),
            "CUDA free detected: dptr: dptr-0", "CUDA free detected: dptr: dptr-1",
        ]
        open(path, "w").write("\n".join(lines) + "\n")
        return 1 + 2 + 2 + 1 + 1 + 2

    if mode == "roundtrip":
        invn = repr(1.0 / n)
        lines = [_malloc("dptr-%d" % i, vec_bytes) for i in (0, 1, 2)]
        lines += [_malloc("dptr-3", tw_bytes), _malloc("dptr-4", tw_bytes)]
        lines += [
            _h2d("dptr-0", vec_bytes, "fft_%s_in_%d.data" % (kind, n)),
            _h2d("dptr-3", tw_bytes, "fft_tw_%d.data" % (n // 2)),
            _h2d("dptr-4", tw_bytes, "fft_twc_%d.data" % (n // 2)),
            _launch_line("fft_bitrev", p["bitrev"], bitrev_grid, block, 0,
                         "dptr-1/8/dptr-0/8/%d/4/%d/4/" % (n, logn)),
        ]
        for s in range(1, logn + 1):   # forward stages (normal twiddles)
            lines.append(_launch_line("fft_stage", p["stage"], stage_grid, block, 0,
                                      "dptr-1/8/dptr-3/8/%d/4/%d/4/%d/4/" % (n, s, logn)))
        lines.append(_launch_line("fft_bitrev", p["bitrev"], bitrev_grid, block, 0,
                                  "dptr-2/8/dptr-1/8/%d/4/%d/4/" % (n, logn)))
        for s in range(1, logn + 1):   # inverse stages (conjugated twiddles)
            lines.append(_launch_line("fft_stage", p["stage"], stage_grid, block, 0,
                                      "dptr-2/8/dptr-4/8/%d/4/%d/4/%d/4/" % (n, s, logn)))
        lines.append(_launch_line("fft_scale", p["scale"], bitrev_grid, block, 0,
                                  "dptr-2/8/%d/4/%s/%d/" % (n, invn, PREC.comp)))
        lines.append(_d2h("dptr-2", vec_bytes, "fft_%s_in_%d.data" % (kind, n)))
        lines += ["CUDA free detected: dptr: dptr-%d" % i for i in range(5)]
        open(path, "w").write("\n".join(lines) + "\n")
        return 1 + 5 + 3 + (2 * (1 + logn)) + 1 + 1 + 5

    # staged (default)
    lines = [_malloc("dptr-0", vec_bytes), _malloc("dptr-1", vec_bytes),
             _malloc("dptr-2", tw_bytes),
             _h2d("dptr-0", vec_bytes, "fft_%s_in_%d.data" % (kind, n)),
             _h2d("dptr-2", tw_bytes, "fft_tw_%d.data" % (n // 2)),
             _launch_line("fft_bitrev", p["bitrev"], bitrev_grid, block, 0,
                          "dptr-1/8/dptr-0/8/%d/4/%d/4/" % (n, logn))]
    for s in range(1, logn + 1):
        lines.append(_launch_line("fft_stage", p["stage"], stage_grid, block, 0,
                                  "dptr-1/8/dptr-2/8/%d/4/%d/4/%d/4/" % (n, s, logn)))
    lines.append(_d2h("dptr-1", vec_bytes, "fft_%s_out_%d.data" % (kind, n)))
    lines += ["CUDA free detected: dptr: dptr-%d" % i for i in range(3)]
    open(path, "w").write("\n".join(lines) + "\n")
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
                    help="compare a GPU D2H dump to a host reference")
    ap.add_argument("--kind", default="tone",
                    help="vector kind for --compare (default: tone)")
    ap.add_argument("--roundtrip", action="store_true",
                    help="with --compare, treat the dump as IFFT(FFT(x)) and "
                         "check it recovers the input")
    ap.add_argument("--double", action="store_true",
                    help="double precision (double2): 16 B/point, f64 .data, "
                         "double2 kernel names")
    args = ap.parse_args()

    global PREC
    PREC = Prec(args.double)

    n = args.n
    logn = int(round(math.log2(n)))
    assert (1 << logn) == n, "N must be a power of two"
    tw = twiddles(n)
    twc = conj_twiddles(n)

    if args.compare:
        ok, info = compare_dump(args.compare, args.kind, n, roundtrip=args.roundtrip)
        print("[compare] kind=%s N=%d : %s" % (args.kind, n, info))
        print("RESULT:", "PASS" if ok else "FAIL")
        return 0 if ok else 1

    ok = True
    for kind in ("impulse", "dc"):
        got = fft_staged(vector(kind, n), tw, n, logn)
        exp = expected_exact(kind, n)
        exact = all(fcast(g.real) == fcast(e.real) and fcast(g.imag) == fcast(e.imag)
                    for g, e in zip(got, exp))
        ok = ok and exact
        print("[{:8s}] N={:<5d} staged==exact: {}".format(kind, n, exact))

    # The tone drives every twiddle, so it validates the FFT math the way impulse
    # cannot; the round-trip IFFT(FFT(x)) is a self-consistent check needing no
    # external reference.
    for kind in ("tone", "ramp"):
        a = fft_staged(vector(kind, n), tw, n, logn)
        d = naive_dft(vector(kind, n), n)
        rel = max(abs(a[k] - d[k]) for k in range(n)) / (max(abs(v) for v in d) or 1.0)
        print("[{:8s}] N={:<5d} staged-vs-DFT max rel err: {:.3e}".format(kind, n, rel))
        ok = ok and rel < 1e-4
    xr = ifft_staged(fft_staged(vector("tone", n), tw, n, logn), twc, n, logn)
    xin = vector("tone", n)
    rt_rel = max(abs(xr[k] - xin[k]) for k in range(n)) / (max(abs(v) for v in xin) or 1.0)
    print("[roundtrip] N={:<5d} IFFT(FFT(x)) max rel err: {:.3e}".format(n, rt_rel))
    ok = ok and rt_rel < 1e-4

    if args.emit:
        os.makedirs(args.outdir, exist_ok=True)
        write_complex(os.path.join(args.outdir, "fft_tw_%d.data" % (n // 2)), tw)
        write_complex(os.path.join(args.outdir, "fft_twc_%d.data" % (n // 2)), twc)
        for kind in ("impulse", "dc", "tone"):
            write_complex(os.path.join(args.outdir, "fft_%s_in_%d.data" % (kind, n)),
                          vector(kind, n))
            write_complex(os.path.join(args.outdir, "fft_%s_out_%d.data" % (kind, n)),
                          reference_output(kind, n, tw, logn))
            if not args.data_only:
                for m in ("staged", "shared"):
                    suffix = "" if m == "staged" else "_shared"
                    write_trace(os.path.join(
                        args.outdir, "fft_%s_%d%s.trace" % (kind, n, suffix)),
                        kind, n, logn, mode=m)
        # Round-trip trace uses the tone (gold = the input it must recover).
        if not args.data_only:
            write_trace(os.path.join(args.outdir, "fft_roundtrip_%d.trace" % n),
                        "tone", n, logn, mode="roundtrip")
        print("emitted %s files (%s) to %s" % (
            ".data" if args.data_only else ".data + .trace", PREC.name,
            os.path.abspath(args.outdir)))

    if args.header:
        write_firmware_header(args.header, n, logn)
        print("wrote firmware twiddle header:", os.path.abspath(args.header))

    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
