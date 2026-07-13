# -*- coding: utf-8 -*-
"""
expanded_coldfire_tests.py — completeness/robustness sweep for the ColdFire
(mcf5208evb) stack. The SST harness only auto-discovers testsuite_*.py, so CI
and `make check-quetz-integration` invoke this gated companion explicitly:

    export PATH=/opt/sst/bin:$PATH LD_LIBRARY_PATH=/opt/sst/lib SST_HOME=/opt/sst
    cd .../quetz/tests
    sst-test-elements -p expanded_coldfire_tests.py

or a subset:

    sst-test-elements -p expanded_coldfire_tests.py -e test_xt_irq_burst

Firmware lives under sysmode/firmware/expanded/ with its own build.sh
(M68K_CC=m68k-linux-gnu-gcc ./build.sh from that directory) -- separate from
the main firmware set on purpose, since these binaries exist only to stress
specific corners, not as a supported example to build against.

Each test targets a specific completeness question raised in
review-coldfire-stack-correctness.md:

  test_xt_irq_burst              - GPU completion IRQ event counting under
                                    multiple retires before an ack (finding 1a)
  test_xt_irq_zero_latency       - zero-latency completions must still raise
                                    the completion IRQ (startKernel latency==0)
  test_xt_stream_irq_ack_early   - true-level stream IRQ remains asserted when
                                    a consumer acks while data remains
  test_xt_be_alias_default       - explicit legacy LE window byte layout;
                                    documents pre-existing sub-word aliasing
  test_xt_be_alias_opt_in        - window_big_endian=1 + matching kernel flag:
                                    byte layout matches real BE hardware
  test_xt_be_alias_mismatched    - window_big_endian=1 but kernel flag left at
                                    default: the documented footgun, byte-swapped
  test_xt_scale_stress           - 512-sample edge-value sweep through the
                                    chunked kernel-DMA path (general robustness)
  test_xt_dma_window_escape      - guest-programmed kernel-op buffers outside
                                    the SST window are rejected non-fatally
                                    (dma_range_start/end), never a sim abort
  test_xt_kernel_dma_zero_latency - zero-latency kernel DMA reaches writeback
  test_xt_kernel_overflow_reject - ScaleOffset and FFT input ceilings reject
                                    over-limit arg2 values
  test_xt_fft_offload_be_window  - QUETZ_WIN_BIG_ENDIAN=1 FFT offload stays
                                    bit-exact (gates the finding-2 opt-in path)
  test_xt_accel_scale_be_window  - QUETZ_WIN_BIG_ENDIAN=1 scale/offset stays
                                    bit-exact incl. saturation edges
  test_xt_wild_access            - guest access outside every region handler:
                                    must not fatal the simulator (finding 3)
  test_xt_doorbell_flood         - queue-full doorbell drop path under a flood
                                    (general robustness, not tied to a fix)
  test_xt_smp_guard_fatal        - system_mode=1 + vcpu_count=2 must fatal with
                                    a clear message, not silently corrupt (finding 5)
  test_xt_kernel_posted_doorbell_reject - kernel slot populated with
                                    doorbell_blocking=0 must fatal at
                                    construction (documents an existing,
                                    NOT-yet-fixed guardrail from the original
                                    review's finding #7)

Some tests deliberately pin supported legacy/misconfiguration behavior (the
explicit LE alias case and mismatched flags), but every test must pass.
"""

from sst_unittest import *
from sst_unittest_support import *
import os
import re

from quetz_test_helpers import make_sysmode_env, stat_sum, enable_mmio_payload_delivery


def sst_paths():
    """Return (prefix, bindir, libexecdir) from the SST simulator config."""
    return (
        sstsimulator_conf_get_value("SSTCore", "prefix", str, ""),
        sstsimulator_conf_get_value("SSTCore", "bindir", str, ""),
        sstsimulator_conf_get_value("SSTCore", "libexecdir", str, ""),
    )


class testcase_expanded_coldfire(SSTTestCase):

    def setUp(self):
        super(type(self), self).setUp()

    def tearDown(self):
        super(type(self), self).tearDown()

    # -------------------------------------------------------------------------
    def _qemu_system_m68k(self):
        sst_prefix, sst_bindir, sst_libexec = sst_paths()
        import shutil
        qemu_bin = os.path.join(sst_bindir, "qemu-system-m68k")
        if not os.path.exists(qemu_bin):
            found = shutil.which("qemu-system-m68k")
            if found:
                qemu_bin = found
        if not os.path.exists(qemu_bin):
            self.skipTest("qemu-system-m68k not found; rebuild QEMU with m68k-softmmu")
        return sst_prefix, sst_bindir, sst_libexec, qemu_bin

    def _expanded_fw(self, test_path, name):
        exe_abs = os.path.normpath(os.path.join(
            test_path, "sysmode/firmware/expanded", name))
        if not os.path.exists(exe_abs):
            self.skipTest(
                "{} not found at {}; run (cd sysmode/firmware/expanded && "
                "M68K_CC=m68k-linux-gnu-gcc ./build.sh)".format(name, exe_abs))
        return exe_abs

    def _main_fw(self, test_path, name):
        exe_abs = os.path.normpath(os.path.join(
            test_path, "sysmode/firmware", name))
        if not os.path.exists(exe_abs):
            self.skipTest(
                "{} not found at {}; run (cd sysmode/firmware && "
                "M68K_CC=m68k-linux-gnu-gcc ./build.sh)".format(name, exe_abs))
        return exe_abs

    def _read(self, path):
        if os.path.exists(path):
            with open(path, "r") as f:
                return f.read()
        return ""

    # -------------------------------------------------------------------------
    def test_xt_irq_burst(self):
        """GPU completion-IRQ event counting: several non-blocking doorbells
        fired back-to-back before any ack. Pre-fix, only the first
        completion's IRQ was ever observable and phase 1 hangs (the SST run
        ends on its own once nothing is left to do, and the report/PASS
        lines never print). Post-fix, every completion is eventually
        observed and a single wildcard ack drains everything at once."""
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec, qemu_bin = self._qemu_system_m68k()
        exe_abs = self._expanded_fw(test_path, "coldfire_xt_irq_burst")

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "expanded_coldfire_tests", "irq_burst")
        os.makedirs(outdir, exist_ok=True)

        make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                         "-machine mcf5208evb -display none -serial stdio -m 128M",
                         "-kernel", 0x40000000, 0x47FFFFFF, [],
                         stdin_file="/dev/null")
        os.environ["QUETZ_MMIO_START"] = "0x70000000"
        os.environ["QUETZ_MMIO_END"]   = "0x7001FFFF"
        os.environ["QUETZ_GPU_IRQ_LINE"] = "30"
        enable_mmio_payload_delivery()

        sst_outfile = os.path.join(outdir, "irq_burst.out")
        sst_errfile = os.path.join(outdir, "irq_burst.err")
        mpifiles    = os.path.join(outdir, "irq_burst.testfile")

        try:
            self.run_sst(os.path.join(test_path, "sysmode",
                                      "basic_quetz_coldfire_system.py"),
                         sst_outfile, sst_errfile,
                         mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=120)
        finally:
            os.environ.pop("QUETZ_GPU_IRQ_LINE", None)
            os.environ.pop("QUETZ_IRQ_LINES", None)

        raw = self._read(sst_outfile) + "\n" + self._read(sst_errfile)
        self.assertNotIn("FATAL", raw)
        self.assertIn("phase1: accel_irqs=5 kernel_id=5", raw,
            "not all 5 back-to-back completions were observed before ack — "
            "the event-counting IRQ fix may have regressed")
        self.assertIn("irq_ack_after_wildcard=0", raw,
            "wildcard ack (~0) did not drain all outstanding completion events")
        self.assertIn("IRQ BURST PASS", raw)
        self.assertIn("TESTFINISH[0]", raw)

    # -------------------------------------------------------------------------
    def test_xt_irq_zero_latency(self):
        """A ZERO-latency kernel completion must still raise the completion
        IRQ. startKernel()'s latency==0 shortcut used to bump kernel_id
        without calling raiseIrqOnRetire(), so an ISR-driven guest submitting
        with LATENCY_OVR=0 slept forever; pre-fix this hangs at the first
        cf_wait_until and the report/PASS lines never print.

        Phase 2 covers the pending-queue wedge: retireIfReady() used to pop
        only ONE queued doorbell per retire, so a zero-latency pop (which
        leaves the device un-busy) stranded everything queued behind it —
        kernel_id froze and the device held the sim open until timeout."""
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec, qemu_bin = self._qemu_system_m68k()
        exe_abs = self._expanded_fw(test_path, "coldfire_xt_irq_zero_latency")

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "expanded_coldfire_tests", "irq_zero_latency")
        os.makedirs(outdir, exist_ok=True)

        make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                         "-machine mcf5208evb -display none -serial stdio -m 128M",
                         "-kernel", 0x40000000, 0x47FFFFFF, [],
                         stdin_file="/dev/null")
        os.environ["QUETZ_MMIO_START"] = "0x70000000"
        os.environ["QUETZ_MMIO_END"]   = "0x7001FFFF"
        os.environ["QUETZ_GPU_IRQ_LINE"] = "30"
        enable_mmio_payload_delivery()

        sst_outfile = os.path.join(outdir, "irq_zero_latency.out")
        sst_errfile = os.path.join(outdir, "irq_zero_latency.err")
        mpifiles    = os.path.join(outdir, "irq_zero_latency.testfile")

        try:
            self.run_sst(os.path.join(test_path, "sysmode",
                                      "basic_quetz_coldfire_system.py"),
                         sst_outfile, sst_errfile,
                         mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=120)
        finally:
            os.environ.pop("QUETZ_GPU_IRQ_LINE", None)
            os.environ.pop("QUETZ_IRQ_LINES", None)

        raw = self._read(sst_outfile) + "\n" + self._read(sst_errfile)
        self.assertNotIn("FATAL", raw)
        self.assertIn("zero-latency: accel_irqs=3 kernel_id=3", raw,
            "zero-latency completions were not all IRQ-observable — the "
            "startKernel latency==0 path may have lost raiseIrqOnRetire again")
        self.assertIn("queued-zero-latency: accel_irqs=6 kernel_id=6", raw,
            "doorbells queued behind a zero-latency pop were stranded — "
            "retireIfReady's drain-while-unbusy loop may have regressed")
        self.assertIn("ZERO LATENCY IRQ PASS", raw)
        self.assertIn("TESTFINISH[0]", raw)

    # -------------------------------------------------------------------------
    def test_xt_stream_irq_ack_early(self):
        """True-level stream IRQ: the ISR pops one word, then ACKs while most
        of the refill remains. The line must stay high and re-enter without
        waiting for another paced refill."""
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec, qemu_bin = self._qemu_system_m68k()
        exe_abs = self._expanded_fw(test_path, "coldfire_xt_stream_ack_early")

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "expanded_coldfire_tests", "stream_ack_early")
        os.makedirs(outdir, exist_ok=True)

        make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                         "-machine mcf5208evb -display none -serial stdio -m 128M",
                         "-kernel", 0x40000000, 0x47FFFFFF, [],
                         stdin_file="/dev/null")
        os.environ["QUETZ_MMIO_START"] = "0x70000000"
        os.environ["QUETZ_MMIO_END"]   = "0x7001FFFF"
        os.environ["QUETZ_SENSOR_IRQ_LINE"] = "31"
        os.environ["QUETZ_SENSOR_PACE_BYTES"]  = "32"   # 8 words/period
        os.environ["QUETZ_SENSOR_PACE_PERIOD"] = "50us"
        enable_mmio_payload_delivery()

        sst_outfile = os.path.join(outdir, "stream_ack_early.out")
        sst_errfile = os.path.join(outdir, "stream_ack_early.err")
        mpifiles    = os.path.join(outdir, "stream_ack_early.testfile")

        try:
            self.run_sst(os.path.join(test_path, "sysmode",
                                      "basic_quetz_coldfire_system.py"),
                         sst_outfile, sst_errfile,
                         mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=120)
        finally:
            os.environ.pop("QUETZ_SENSOR_IRQ_LINE", None)
            os.environ.pop("QUETZ_IRQ_LINES", None)
            os.environ.pop("QUETZ_SENSOR_PACE_BYTES", None)
            os.environ.pop("QUETZ_SENSOR_PACE_PERIOD", None)

        raw = self._read(sst_outfile) + "\n" + self._read(sst_errfile)
        self.assertNotIn("FATAL", raw)
        self.assertIn("STREAM ACK-EARLY PASS", raw,
            "guest did not re-enter while stream data remained pending")
        self.assertIn("TESTFINISH[0]", raw)

        m = re.search(r"sensor_irqs=(\d+)", raw)
        self.assertIsNotNone(m, "sensor_irqs count missing from transcript")
        self.assertGreaterEqual(int(m.group(1)), 4,
            "fewer than 4 independent wakeups observed")

    # -------------------------------------------------------------------------
    def _be_alias_run(self, outdir_name, win_big_endian, kernel_big_endian):
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec, qemu_bin = self._qemu_system_m68k()
        exe_abs = self._expanded_fw(test_path, "coldfire_xt_be_alias")

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "expanded_coldfire_tests", outdir_name)
        os.makedirs(outdir, exist_ok=True)

        sdlfile = os.path.join(test_path, "sysmode",
                               "basic_quetz_gpu_compute_coldfire.py")
        sst_outfile = os.path.join(outdir, outdir_name + ".out")
        sst_errfile = os.path.join(outdir, outdir_name + ".err")
        mpifiles    = os.path.join(outdir, outdir_name + ".testfile")

        make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                         "-machine mcf5208evb -display none -serial stdio -m 128M",
                         "-kernel", 0x40000000, 0x47FFFFFF, [])
        os.environ["QUETZ_MMIO_START"] = "0x70000000"
        os.environ["QUETZ_MMIO_END"]   = "0x700003FF"
        os.environ["QUETZ_SST_WIN_START"] = "0x71000000"
        os.environ["QUETZ_SST_WIN_END"]   = "0x7100FFFF"
        os.environ["QUETZ_KERNEL"] = "quetz.ScaleOffsetKernel"
        os.environ["QUETZ_WIN_BIG_ENDIAN"] = "1" if win_big_endian else "0"
        os.environ["QUETZ_KERNEL_BIG_ENDIAN"] = "1" if kernel_big_endian else "0"
        # The GPU register range is served by the sync-MMIO bridge; without
        # QUETZ_MMIO_PAYLOAD=1 the launcher instantiates no bridge for it and
        # the ARG/doorbell writes take the imprecise trace path (garbage DMA
        # addresses -> MemNIC fatal).
        enable_mmio_payload_delivery()

        try:
            self.run_sst(sdlfile, sst_outfile, sst_errfile,
                         mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=120)
        finally:
            os.environ.pop("QUETZ_SST_WIN_START", None)
            os.environ.pop("QUETZ_SST_WIN_END", None)
            os.environ.pop("QUETZ_WIN_BIG_ENDIAN", None)
            os.environ.pop("QUETZ_KERNEL_BIG_ENDIAN", None)
            os.environ.pop("QUETZ_KERNEL", None)

        return self._read(sst_outfile) + "\n" + self._read(sst_errfile)

    def test_xt_be_alias_default(self):
        """Default (window_big_endian=0, kernel data_big_endian=0):
        documents the PRE-EXISTING sub-word aliasing this whole feature
        exists to address -- a byte-write hi-then-lo (the natural
        big-endian convention) read back as one word comes back
        BYTE-SWAPPED. This assertion passing is NOT a bug; it is the
        expected, unchanged default behavior."""
        raw = self._be_alias_run("be_alias_default",
                                 win_big_endian=False, kernel_big_endian=False)
        self.assertNotIn("FATAL", raw)
        self.assertIn("TESTFINISH[0]", raw)
        self.assertIn("word_read=0x00003412", raw,
            "explicit legacy LE window packing should byte-swap a BE-natural "
            "byte-write-then-word-read (0x1234 written hi,lo -> 0x3412 read)")
        # Value-changing kernel probe: everything is consistently LE end to
        # end, so the kernel sees 0x3412, adds 0xCC, and the guest's LE
        # word-read composes the same 0x34DE back.
        self.assertIn("kernel offset roundtrip (scale=1/offset=0xCC): 0x000034de",
                      raw)

    def test_xt_be_alias_opt_in(self):
        """window_big_endian=1 + kernel data_big_endian=1 (matched): byte
        layout matches real BE hardware, and the full kernel round-trip
        (identity scale=1/offset=0) comes back correct."""
        raw = self._be_alias_run("be_alias_opt_in",
                                 win_big_endian=True, kernel_big_endian=True)
        self.assertNotIn("FATAL", raw)
        self.assertIn("TESTFINISH[0]", raw)
        self.assertIn("word_read=0x00001234", raw,
            "window_big_endian=1 should preserve BE-natural byte layout "
            "(0x1234 written hi,lo -> 0x1234 read back)")
        self.assertIn("kernel roundtrip (identity scale=1/offset=0): 0x00001234", raw,
            "kernel round-trip should be exact when data_big_endian matches "
            "window_big_endian")
        # Value-changing probe, matched flags: 0x1234 + 0xCC = 0x1300 —
        # exactly what real BE hardware would produce.
        self.assertIn("kernel offset roundtrip (scale=1/offset=0xCC): 0x00001300",
                      raw)

    def test_xt_be_alias_mismatched(self):
        """window_big_endian=1 on the CPU but the kernel's data_big_endian
        left at 0 (via the independent QUETZ_KERNEL_BIG_ENDIAN override):
        documents the footgun the param text warns about -- setting one
        flag without the other silently scrambles the kernel's view of the
        data. NOT a bug in itself; a completeness check that the documented
        failure mode is real and reproducible."""
        raw = self._be_alias_run("be_alias_mismatched",
                                 win_big_endian=True, kernel_big_endian=False)
        self.assertNotIn("FATAL", raw)
        self.assertIn("TESTFINISH[0]", raw)
        # The raw byte/word probe only involves the CPU side, so it is
        # unaffected by the kernel's flag -- still correct BE layout.
        self.assertIn("word_read=0x00001234", raw)
        # An IDENTITY round-trip cannot expose the mismatch: the kernel
        # misreads the bytes byte-swapped but miswrites them straight back
        # (swap . identity . swap == identity), so 0x1234 returns intact.
        self.assertIn("kernel roundtrip (identity scale=1/offset=0): 0x00001234", raw)
        # The value-changing probe is where the mismatch bites: the kernel
        # reads the sample LE as 0x3412, adds 0xCC (-> 0x34DE), and writes
        # the result back LE (bytes 0xDE,0x34); the BE guest word-reads
        # 0xDE34 instead of the 0x1300 real hardware would produce.
        self.assertIn("kernel offset roundtrip (scale=1/offset=0xCC): 0x0000de34",
                      raw,
            "expected the mismatched-flags footgun to scramble the "
            "value-changing kernel round-trip")

    # -------------------------------------------------------------------------
    def test_xt_scale_stress(self):
        """512 synthetic samples (16x the existing accel_scale test's fixture
        size) with forced INT16_MIN/MAX/0/-1 plus a full-range ramp, run
        through the chunked kernel-DMA path (kOpDmaChunk=64B -> 16 chunks
        each direction) under the ColdFire default (BE) window. Integration-level
        confirmation beyond the host unit tests that already cover the pure
        math."""
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec, qemu_bin = self._qemu_system_m68k()
        exe_abs = self._expanded_fw(test_path, "coldfire_xt_scale_stress")

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "expanded_coldfire_tests", "scale_stress")
        os.makedirs(outdir, exist_ok=True)

        sdlfile = os.path.join(test_path, "sysmode",
                               "basic_quetz_gpu_compute_coldfire.py")
        sst_outfile = os.path.join(outdir, "scale_stress.out")
        sst_errfile = os.path.join(outdir, "scale_stress.err")
        mpifiles    = os.path.join(outdir, "scale_stress.testfile")

        make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                         "-machine mcf5208evb -display none -serial stdio -m 128M",
                         "-kernel", 0x40000000, 0x47FFFFFF, [])
        os.environ["QUETZ_MMIO_START"] = "0x70000000"
        os.environ["QUETZ_MMIO_END"]   = "0x700003FF"
        os.environ["QUETZ_SST_WIN_START"] = "0x71000000"
        os.environ["QUETZ_SST_WIN_END"]   = "0x7100FFFF"
        os.environ["QUETZ_KERNEL"] = "quetz.ScaleOffsetKernel"
        os.environ.pop("QUETZ_WIN_BIG_ENDIAN", None)
        os.environ.pop("QUETZ_KERNEL_BIG_ENDIAN", None)
        # Sync-MMIO bridge for the GPU registers (see _be_alias_run).
        enable_mmio_payload_delivery()

        try:
            self.run_sst(sdlfile, sst_outfile, sst_errfile,
                         mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=180)
        finally:
            os.environ.pop("QUETZ_SST_WIN_START", None)
            os.environ.pop("QUETZ_SST_WIN_END", None)
            os.environ.pop("QUETZ_KERNEL", None)

        raw = self._read(sst_outfile) + "\n" + self._read(sst_errfile)
        self.assertNotIn("FATAL", raw)
        self.assertIn("SCALE STRESS PASS", raw,
            "512-sample edge-value sweep through the chunked DMA path failed")
        self.assertIn("TESTFINISH[0]", raw)

        m = re.search(r"correct=(\d+)/512", raw)
        self.assertIsNotNone(m, "correct-sample count missing from transcript")
        self.assertEqual(int(m.group(1)), 512)

    # -------------------------------------------------------------------------
    def _compute_deck_run(self, exe_abs, outdir_name, extra_env=None,
                          timeout_sec=120):
        """One basic_quetz_gpu_compute_coldfire.py run with the standard
        MMIO/window env; extra_env entries are set for the run and popped
        after."""
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec, qemu_bin = self._qemu_system_m68k()

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "expanded_coldfire_tests", outdir_name)
        os.makedirs(outdir, exist_ok=True)

        sdlfile = os.path.join(test_path, "sysmode",
                               "basic_quetz_gpu_compute_coldfire.py")
        sst_outfile = os.path.join(outdir, outdir_name + ".out")
        sst_errfile = os.path.join(outdir, outdir_name + ".err")
        mpifiles    = os.path.join(outdir, outdir_name + ".testfile")

        make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                         "-machine mcf5208evb -display none -serial stdio -m 128M",
                         "-kernel", 0x40000000, 0x47FFFFFF, [])
        os.environ["QUETZ_MMIO_START"] = "0x70000000"
        os.environ["QUETZ_MMIO_END"]   = "0x700003FF"
        os.environ["QUETZ_SST_WIN_START"] = "0x71000000"
        os.environ["QUETZ_SST_WIN_END"]   = "0x7100FFFF"
        extra_env = extra_env or {}
        for k, v in extra_env.items():
            os.environ[k] = v
        enable_mmio_payload_delivery()

        try:
            self.run_sst(sdlfile, sst_outfile, sst_errfile,
                         mpi_out_files=mpifiles, set_cwd=outdir,
                         timeout_sec=timeout_sec)
        finally:
            os.environ.pop("QUETZ_SST_WIN_START", None)
            os.environ.pop("QUETZ_SST_WIN_END", None)
            for k in extra_env:
                os.environ.pop(k, None)

        return self._read(sst_outfile) + "\n" + self._read(sst_errfile)

    def test_xt_dma_window_escape(self):
        """Guest-programmed kernel-op registers must not crash the simulator.
        Pre-fix: src/dst outside the SST window sent kernel DMA to an address
        no memHierarchy endpoint owns (MemNIC routing FATAL) and N=0 hit an
        out.fatal. Post-fix (dma_range_start/end on the deck): 4 bad ops are
        rejected non-fatally (blocking doorbell completes, KERNEL_ID stays 0,
        gpu.ops_rejected counts them) and a valid op still runs afterwards."""
        test_path = self.get_testsuite_dir()
        exe_abs = self._expanded_fw(test_path, "coldfire_xt_dma_escape")
        raw = self._compute_deck_run(
            exe_abs, "dma_escape",
            extra_env={"QUETZ_KERNEL": "quetz.ScaleOffsetKernel"})

        self.assertNotIn("FATAL", raw,
            "a guest-programmed DMA address aborted the simulator — the "
            "dma_range rejection may have regressed")
        self.assertIn("rejected probes: kernel_id=0,0,0,0", raw,
            "a rejected op advanced KERNEL_ID (or a reject hung the doorbell)")
        self.assertIn("valid op after rejects: kernel_id=1 correct=64/64", raw,
            "the device did not run a valid op correctly after rejections")
        self.assertIn("DMA ESCAPE PASS", raw)
        self.assertIn("TESTFINISH[0]", raw)

        self.assertEqual(stat_sum(raw, "gpu.ops_rejected", 0), 4,
            "expected exactly 4 rejected ops (N=0, wild src, src straddle, "
            "dst straddle)")
        # The dst-straddle op is only caught at writeback, after compute
        # counted it as launched — so 2 launches (it + the valid op).
        self.assertEqual(stat_sum(raw, "gpu.kernels_launched", 0), 2)

    # -------------------------------------------------------------------------
    def test_xt_kernel_dma_zero_latency(self):
        """A real kernel op with zero kernel and device fallback latency must
        enter DMA writeback immediately instead of storing the idle sentinel
        in busy_until_clk_."""
        test_path = self.get_testsuite_dir()
        exe_abs = self._main_fw(test_path, "coldfire_accel_scale")
        raw = self._compute_deck_run(
            exe_abs, "kernel_dma_zero_latency",
            extra_env={"QUETZ_KERNEL": "quetz.ScaleOffsetKernel",
                       "QUETZ_SCALE_LATENCY_COEFF": "0",
                       "QUETZ_GPU_LATENCY": "0",
                       "QUETZ_GPU_CLOCK": "1Hz"})

        self.assertNotIn("FATAL", raw)
        self.assertIn("accel scale correct_samples=64/64", raw,
            "zero-latency kernel DMA did not reach writeback")
        self.assertIn("TESTFINISH[0]", raw)
        self.assertEqual(stat_sum(raw, "gpu.kernels_launched", 0), 1)
        self.assertEqual(stat_sum(raw, "gpu.busy_cycles", 0), 0)

    # -------------------------------------------------------------------------
    def test_xt_kernel_overflow_reject(self):
        """Both kernel implementations reject an arg2 just beyond their input
        ceiling before allocation, multiplication, or DMA."""
        test_path = self.get_testsuite_dir()
        cases = (
            ("scale", "coldfire_xt_scale_overflow",
             "quetz.ScaleOffsetKernel"),
            ("fft", "coldfire_xt_fft_overflow", "quetz.FFTKernel"),
        )
        for label, firmware, kernel in cases:
            with self.subTest(kernel=label):
                raw = self._compute_deck_run(
                    self._expanded_fw(test_path, firmware),
                    "kernel_overflow_" + label,
                    extra_env={"QUETZ_KERNEL": kernel})
                self.assertNotIn("FATAL", raw)
                self.assertIn("KERNEL OVERFLOW REJECT PASS", raw)
                self.assertIn("TESTFINISH[0]", raw)
                self.assertEqual(stat_sum(raw, "gpu.ops_rejected", 0), 1)
                self.assertEqual(stat_sum(raw, "gpu.kernels_launched", 0), 0)

    # -------------------------------------------------------------------------
    def test_xt_fft_offload_be_window(self):
        """Gate the BE-window FFT offload path (finding #2's opt-in fix on
        quetz.FFTKernel): the packed-u32 fft_offload firmware must stay
        bit-exact with QUETZ_WIN_BIG_ENDIAN=1 (window_big_endian on the CPU +
        data_big_endian on the kernel, wired in lockstep by the deck).
        Promotes the manual second-pass verification run into the sweep."""
        test_path = self.get_testsuite_dir()
        exe_abs = self._main_fw(test_path, "coldfire_gpu_fft_offload")
        raw = self._compute_deck_run(
            exe_abs, "fft_offload_be_window",
            extra_env={"QUETZ_WIN_BIG_ENDIAN": "1"},
            timeout_sec=180)

        self.assertNotIn("FATAL", raw)
        self.assertIn("GPU FFT offload correct_words=512/512", raw,
            "device-computed FFT not bit-exact under the BE window layout")
        self.assertIn("TESTFINISH[0]", raw)

    # -------------------------------------------------------------------------
    def test_xt_accel_scale_be_window(self):
        """Gate the BE-window scale/offset path (quetz.ScaleOffsetKernel's
        data_big_endian): the packed-u32 accel_scale firmware must stay
        bit-exact (incl. saturation edges) with QUETZ_WIN_BIG_ENDIAN=1.
        Promotes the manual second-pass verification run into the sweep."""
        test_path = self.get_testsuite_dir()
        exe_abs = self._main_fw(test_path, "coldfire_accel_scale")
        raw = self._compute_deck_run(
            exe_abs, "accel_scale_be_window",
            extra_env={"QUETZ_KERNEL": "quetz.ScaleOffsetKernel",
                       "QUETZ_WIN_BIG_ENDIAN": "1"},
            timeout_sec=180)

        self.assertNotIn("FATAL", raw)
        self.assertIn("accel scale correct_samples=64/64", raw,
            "device-computed scale/offset not bit-exact under the BE window "
            "layout")
        self.assertIn("TESTFINISH[0]", raw)

    # -------------------------------------------------------------------------
    def test_xt_wild_access(self):
        """Guest access outside every region handler (RAM, MMIO window,
        UART, sentinel) must not abort the SST simulator. Probes addresses
        in the gaps basic_quetz_gpu_coldfire.py's new catch-all filter
        covers; the transcript (no FATAL, TESTFINISH reached) is the result,
        like bsp_torture."""
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec, qemu_bin = self._qemu_system_m68k()
        exe_abs = self._expanded_fw(test_path, "coldfire_xt_wild_access")

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "expanded_coldfire_tests", "wild_access")
        os.makedirs(outdir, exist_ok=True)

        sdlfile = os.path.join(test_path, "sysmode", "basic_quetz_gpu_coldfire.py")
        sst_outfile = os.path.join(outdir, "wild_access.out")
        sst_errfile = os.path.join(outdir, "wild_access.err")
        mpifiles    = os.path.join(outdir, "wild_access.testfile")

        make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                         "-machine mcf5208evb -display none -serial stdio -m 128M",
                         "-kernel", 0x40000000, 0x47FFFFFF, [])
        os.environ["QUETZ_MMIO_START"] = "0x70000000"
        os.environ["QUETZ_MMIO_END"]   = "0x700003FF"
        os.environ.pop("QUETZ_VCPU_COUNT", None)
        enable_mmio_payload_delivery()

        self.run_sst(sdlfile, sst_outfile, sst_errfile,
                     mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=120)

        raw = self._read(sst_outfile) + "\n" + self._read(sst_errfile)
        self.assertNotIn("FATAL", raw,
            "a wild guest access reached memHierarchy and aborted the "
            "simulator -- the catch-all region-handler fix may have regressed")
        self.assertIn("wild probe done:", raw)
        self.assertIn("TESTFINISH[0]", raw)

        filtered_total = (stat_sum(raw, "filtered_reads", 0)
                         + stat_sum(raw, "filtered_writes", 0))
        self.assertGreaterEqual(filtered_total, 8,
            "expected the 4 probed addresses x 2 accesses (r32+w32) to land "
            "in the catch-all FilteredRegionHandler")

    # -------------------------------------------------------------------------
    def test_xt_doorbell_flood(self):
        """11 non-blocking doorbells fired back-to-back (more than
        kMaxPendingLaunches=8 + 1 active = 9 acceptable) must not crash the
        device -- excess doorbells are dropped (doorbell_while_busy), not
        fatal. General robustness probe, not tied to a specific fix."""
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec, qemu_bin = self._qemu_system_m68k()
        exe_abs = self._expanded_fw(test_path, "coldfire_xt_doorbell_flood")

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "expanded_coldfire_tests", "doorbell_flood")
        os.makedirs(outdir, exist_ok=True)

        sdlfile = os.path.join(test_path, "sysmode", "basic_quetz_gpu_coldfire.py")
        sst_outfile = os.path.join(outdir, "doorbell_flood.out")
        sst_errfile = os.path.join(outdir, "doorbell_flood.err")
        mpifiles    = os.path.join(outdir, "doorbell_flood.testfile")

        make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                         "-machine mcf5208evb -display none -serial stdio -m 128M",
                         "-kernel", 0x40000000, 0x47FFFFFF, [])
        os.environ["QUETZ_MMIO_START"] = "0x70000000"
        os.environ["QUETZ_MMIO_END"]   = "0x700003FF"
        os.environ.pop("QUETZ_VCPU_COUNT", None)
        # Without the sync-MMIO bridge, STATUS/KERNEL_ID reads are RAZ from
        # QEMU (kernel_id would read 0 forever) and the LATENCY_OVR payloads
        # take the imprecise trace path.
        enable_mmio_payload_delivery()

        self.run_sst(sdlfile, sst_outfile, sst_errfile,
                     mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=120)

        raw = self._read(sst_outfile) + "\n" + self._read(sst_errfile)
        self.assertNotIn("FATAL", raw,
            "the device fataled under a doorbell flood instead of dropping "
            "the excess (see gpu_device.cc's queue-full path)")
        self.assertIn("DOORBELL FLOOD PASS", raw)
        self.assertIn("TESTFINISH[0]", raw)

        self.assertGreaterEqual(stat_sum(raw, "gpu.doorbell_while_busy", 0), 2,
            "expected at least 2 doorbells queued-while-busy or dropped")
        self.assertEqual(stat_sum(raw, "gpu.kernels_launched", 0), 9,
            "expected exactly 9 accepted doorbells (1 active + 8 queued)")

    # -------------------------------------------------------------------------
    def test_xt_smp_guard_fatal(self):
        """system_mode=1 with vcpu_count=2 must fatal at construction with a
        clear message (the sync-MMIO bridge is wired to mailbox slot 0 only;
        two MTTCG vCPU threads would race one request slot). Reuses the
        trivial coldfire_hello binary -- it is never executed, since the
        fatal fires before QEMU is even spawned."""
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec, qemu_bin = self._qemu_system_m68k()
        exe_abs = os.path.normpath(os.path.join(
            test_path, "sysmode/firmware/coldfire_hello"))
        if not os.path.exists(exe_abs):
            self.skipTest("coldfire_hello not found at {}".format(exe_abs))

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "expanded_coldfire_tests", "smp_guard")
        os.makedirs(outdir, exist_ok=True)

        sdlfile = os.path.join(test_path, "sysmode", "basic_quetz_gpu_coldfire.py")
        sst_outfile = os.path.join(outdir, "smp_guard.out")
        sst_errfile = os.path.join(outdir, "smp_guard.err")
        mpifiles    = os.path.join(outdir, "smp_guard.testfile")

        os.environ["QUETZ_EXE"] = exe_abs
        os.environ["QUETZ_QEMU"] = qemu_bin
        os.environ["QUETZ_PLUGIN"] = os.path.join(sst_libexec, "libqemu_sst_plugin.so")
        os.environ["SST_HOME"] = sst_prefix
        os.environ["QUETZ_VCPU_COUNT"] = "2"
        # The deck refuses to load without payload delivery; the SMP guard
        # fatal we're probing fires later (at construction, before QEMU).
        enable_mmio_payload_delivery()

        try:
            self.run_sst(sdlfile, sst_outfile, sst_errfile,
                         mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=60)
        finally:
            os.environ.pop("QUETZ_VCPU_COUNT", None)

        raw = self._read(sst_outfile) + "\n" + self._read(sst_errfile)
        self.assertIn("FATAL", raw,
            "system_mode=1 with vcpu_count=2 should fatal (SMP sync-MMIO "
            "guard) instead of silently running")
        self.assertIn("vcpu_count=1", raw,
            "fatal message should name the single-vCPU requirement")
        self.assertNotIn("TESTFINISH", raw,
            "the guest should never have started executing")

    # -------------------------------------------------------------------------
    def test_xt_kernel_posted_doorbell_reject(self):
        """A 'kernel' subcomponent with doorbell_blocking=0 must fatal at
        device construction (documents an EXISTING guardrail from the
        original review's finding #7 -- not one of the five fixes applied
        this session, just confirming it still catches the misconfiguration
        rather than silently misbehaving)."""
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec, qemu_bin = self._qemu_system_m68k()
        exe_abs = os.path.normpath(os.path.join(
            test_path, "sysmode/firmware/coldfire_gpu_fft_offload"))
        if not os.path.exists(exe_abs):
            self.skipTest("coldfire_gpu_fft_offload not found at {}".format(exe_abs))

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "expanded_coldfire_tests", "kernel_posted_reject")
        os.makedirs(outdir, exist_ok=True)

        sdlfile = os.path.join(test_path, "sysmode",
                               "basic_quetz_gpu_compute_coldfire.py")
        sst_outfile = os.path.join(outdir, "kernel_posted_reject.out")
        sst_errfile = os.path.join(outdir, "kernel_posted_reject.err")
        mpifiles    = os.path.join(outdir, "kernel_posted_reject.testfile")

        os.environ["QUETZ_EXE"] = exe_abs
        os.environ["QUETZ_QEMU"] = qemu_bin
        os.environ["QUETZ_PLUGIN"] = os.path.join(sst_libexec, "libqemu_sst_plugin.so")
        os.environ["QUETZ_QEMU_ARGS"] = ("-machine mcf5208evb -display none "
                                        "-serial stdio -m 128M")
        os.environ["QUETZ_LOADER"] = "-kernel"
        os.environ["SST_HOME"] = sst_prefix
        os.environ["QUETZ_MMIO_START"] = "0x70000000"
        os.environ["QUETZ_MMIO_END"]   = "0x700003FF"
        os.environ["QUETZ_SST_WIN_START"] = "0x71000000"
        os.environ["QUETZ_SST_WIN_END"]   = "0x7100FFFF"
        os.environ["QUETZ_DOORBELL_BLOCKING"] = "0"   # misconfig: kernel + non-blocking
        enable_mmio_payload_delivery()   # standalone determinism (no env leakage)

        try:
            self.run_sst(sdlfile, sst_outfile, sst_errfile,
                         mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=60)
        finally:
            os.environ.pop("QUETZ_SST_WIN_START", None)
            os.environ.pop("QUETZ_SST_WIN_END", None)
            os.environ.pop("QUETZ_DOORBELL_BLOCKING", None)

        raw = self._read(sst_outfile) + "\n" + self._read(sst_errfile)
        self.assertIn("FATAL", raw,
            "a kernel slot with doorbell_blocking=0 should fatal at "
            "construction instead of silently running")
        self.assertIn("doorbell_blocking=1", raw)
        self.assertNotIn("TESTFINISH", raw)
