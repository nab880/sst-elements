# -*- coding: utf-8 -*-
"""
testsuite_default_quetz.py — SST test harness for the Quetz element.

User-mode tests run basic_quetz.py and compare deterministic statistics
against reference files:
  usermode/small/<testname>/sst.stdout.gold

System-mode tests run basic_quetz_sysmode.py and compare against:
  sysmode/small/<testname>/sst.stdout.gold

Timing-sensitive statistics (latency accumulators, cycle counts) are excluded
from comparison via the QuetzStatsFilter; only event-count statistics are
checked.

To regenerate all gold files after a statistics change, set updateFiles=True
and run the tests once.
"""

from sst_unittest import *
from sst_unittest_support import *
from sst_unittest_parameterized import parameterized
import os
import subprocess

from quetz_test_helpers import (
    assert_class_balance,
    apply_usermode_region_handlers,
    compare_gold,
    should_compare_gold,
    enable_mmio_payload_delivery,
    filtered_stat_lines,
    make_sysmode_env,
    make_usermode_env,
    parse_stats,
    stat_sum,
)

module_init = 0
module_sema = threading.Semaphore()
quetz_test_matrix = []
quetz_sysmode_matrix = []

updateFiles = False
# updateFiles = True   # uncomment to regenerate gold files

# ---------------------------------------------------------------------------
# Test matrix
# ---------------------------------------------------------------------------
def build_quetz_test_matrix():
    global quetz_test_matrix
    quetz_test_matrix = []

    # Each entry: (testname, qemu_target, exe_rel, with_l1, isa, timeout_sec)
    # exe_rel is relative to the tests/ directory (self.get_testsuite_dir()).
    vanadis_hello = ("../../vanadis/tests/small"
                     "/basic-io/hello-world/riscv64/hello-world")

    testlist = [
        ("riscv64_hello",   "qemu-riscv64", vanadis_hello,                  False, "",        120),
        ("riscv64_l1cache", "qemu-riscv64", vanadis_hello,                  True,  "",        120),
        ("aarch64_hello",   "qemu-aarch64", "binaries/hello_aarch64",       True,  "aarch64", 120),
        ("x86_64_hello",    "qemu-x86_64",  "binaries/hello_x86_64",        True,  "",        120),
    ]

    for testnum, t in enumerate(testlist, start=1):
        quetz_test_matrix.append((testnum,) + t)

build_quetz_test_matrix()


# ---------------------------------------------------------------------------
# System-mode test matrix
# ---------------------------------------------------------------------------
# Each entry:
#   (testname, qemu_target, exe_rel, qemu_args, loader,
#    ram_start, ram_end, memmaps, uart_echo_input, timeout_sec)
#
# region_handlers is a list of (name, start, end, type) tuples.
# type is filtered | uart | memory (mapped to quetz.*RegionHandler).
# uart_echo_input is None (no echo) or a bytes object to inject via stdin.
# ---------------------------------------------------------------------------
def build_quetz_sysmode_matrix():
    global quetz_sysmode_matrix
    quetz_sysmode_matrix = []

    fw = "sysmode/firmware"

    testlist = [
        ("riscv64_virt_hello",
         "qemu-system-riscv64",
         f"{fw}/riscv_virt_hello",
         "-machine virt -nographic -bios none",
         "-kernel",
         0x00000000, 0xFFFFFFFF,
         [("sub_ram", 0x00000000, 0x7FFFFFFF, "filtered")],  # region_handlers
         None, 120),

        ("uart_echo",
         "qemu-system-riscv64",
         f"{fw}/riscv_virt_uart_echo",
         "-machine virt -nographic -bios none",
         "-kernel",
         0x00000000, 0xFFFFFFFF,
         # uart0 must precede sub_ram: sub_ram covers 0x0-0x7fffffff and would
         # otherwise swallow MMIO at 0x10000000 before uart capture runs.
         [("uart0", 0x10000000, 0x10000FFF, "uart"),
          ("sub_ram", 0x00000000, 0x7FFFFFFF, "filtered")],
         b"ABCDE", 120),

        ("riscv64_virt_gpu_trace",
         "qemu-system-riscv64",
         f"{fw}/riscv_virt_gpu_trace",
         "-machine virt -nographic -bios none",
         "-kernel",
         0x00000000, 0xFFFFFFFF,
         # gpu_mmio must precede sub_ram (same ordering rule as uart_echo).
         [("gpu_mmio", 0x80100000, 0x801003FF, "gpu_trace",
             {"doorbell_offset": 0, "status_offset": 8}),
          ("uart0",    0x10000000, 0x10000FFF, "uart"),
          ("sub_ram",  0x00000000, 0x7FFFFFFF, "filtered")],
         None, 120),

        ("arm_m7_hello",
         "qemu-system-arm",
         f"{fw}/arm_m7_hello",
         "-machine mps2-an500 -nographic "
         "-semihosting-config enable=on,target=native",
         "-kernel",
         0x00000000, 0xFFFFFFFF,
         [("periph", 0x40000000, 0xFFFFFFFF, "filtered")],
         None, 120),

        ("x86_hello",
         "qemu-system-i386",
         f"{fw}/x86_hello",
         "-machine pc -nographic "
         "-device isa-debug-exit,iobase=0x501,iosize=1",
         "-kernel",
         0x00000000, 0xFFFFFFFF,
         [],
         None, 120),
    ]

    for testnum, t in enumerate(testlist, start=1):
        quetz_sysmode_matrix.append((testnum,) + t)

build_quetz_sysmode_matrix()


def gen_custom_name(testcase_func, param_num, param):
    return "{0}_{1:03}_{2}".format(
        testcase_func.__name__,
        int(parameterized.to_safe_name(str(param.args[0]))),
        parameterized.to_safe_name(str(param.args[1])))


def sst_paths():
    """Return (prefix, bindir, libexecdir) from the SST simulator config."""
    return (
        sstsimulator_conf_get_value("SSTCore", "prefix", str, ""),
        sstsimulator_conf_get_value("SSTCore", "bindir", str, ""),
        sstsimulator_conf_get_value("SSTCore", "libexecdir", str, ""),
    )


# ---------------------------------------------------------------------------
class testcase_quetz(SSTTestCase):

    def setUp(self):
        super(type(self), self).setUp()

    def tearDown(self):
        super(type(self), self).tearDown()

    # -------------------------------------------------------------------------
    @parameterized.expand(quetz_test_matrix, name_func=gen_custom_name)
    def test_quetz_usermode(self, testnum, testname, qemu_target, exe_rel,
                            with_l1, isa, timeout_sec):
        # Prebuilt aarch64/x86_64 binaries produce host-glibc-sensitive syscall
        # traces. Their gold files were recorded on the lightweight (Ubuntu
        # 24.04) image; on other hosts (e.g. balar Ubuntu 22.04 amd64), stat
        # counts diverge even though the program output is correct.
        if os.getenv("QUETZ_SKIP_PREBUILT_USERMODE", "0") == "1" \
                and testname in ("aarch64_hello", "x86_64_hello"):
            self.skipTest("QUETZ_SKIP_PREBUILT_USERMODE=1: prebuilt binary "
                          "stats are glibc/host-sensitive; covered by the "
                          "lightweight image where gold was recorded")
        log_debug("Quetz test #{} ({}): qemu={} with_l1={} isa={}".format(
            testnum, testname, qemu_target, with_l1, isa))
        self._quetz_test_template(testnum, testname, qemu_target, exe_rel,
                                   with_l1, isa, timeout_sec)

    # -------------------------------------------------------------------------
    def _quetz_test_template(self, testnum, testname, qemu_target, exe_rel,
                              with_l1, isa, testtimeout=120):
        test_path = self.get_testsuite_dir()   # .../quetz/tests/

        sst_prefix, sst_bindir, sst_libexec = sst_paths()

        qemu_bin = os.path.join(sst_bindir, qemu_target)
        exe_abs  = os.path.normpath(os.path.join(test_path, exe_rel))

        if not os.path.exists(qemu_bin):
            self.skipTest("{} not found at {}; skipping".format(qemu_target, qemu_bin))
        if not os.path.exists(exe_abs):
            self.skipTest("test binary not found at {}; skipping".format(exe_abs))

        outdir      = os.path.join(self.get_test_output_run_dir(),
                                   "quetz_tests", testname)
        os.makedirs(outdir, exist_ok=True)

        sdlfile     = os.path.join(test_path, "usermode", "basic_quetz.py")
        test_label  = "test_quetz_{}".format(testname)
        sst_outfile = os.path.join(outdir, test_label + ".out")
        sst_errfile = os.path.join(outdir, test_label + ".err")
        mpifiles    = os.path.join(outdir, test_label + ".testfile")
        ref_outfile = os.path.join(test_path, "usermode", "small",
                                   testname, "sst.stdout.gold")

        make_usermode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                          with_l1=with_l1, isa=isa, detailed=True)

        oscmd = self.run_sst(sdlfile, sst_outfile, sst_errfile,
                             mpi_out_files=mpifiles,
                             set_cwd=outdir,
                             timeout_sec=testtimeout)

        if os.path.exists(ref_outfile) and should_compare_gold():
            cmp_result = compare_gold(testname, sst_outfile, ref_outfile,
                                      update_files=updateFiles)
            if not cmp_result:
                log_failure(oscmd)
            self.assertTrue(cmp_result,
                "Quetz output {} does not match reference {}".format(
                    sst_outfile, ref_outfile))
        elif os.path.exists(ref_outfile):
            log_testing_note(
                "Quetz test {} gold skipped (QUETZ_SKIP_GOLD=1)".format(testname))
        else:
            log_testing_note(
                "Quetz test {} has no gold file; did not compare".format(testname))

    # -------------------------------------------------------------------------
    # Multicore halt-quorum test: verify both vCPUs ran to completion.
    # -------------------------------------------------------------------------
    def test_quetz_multicore_quorum(self):
        test_path = self.get_testsuite_dir()

        sst_prefix, sst_bindir, sst_libexec = sst_paths()

        qemu_bin = os.path.join(sst_bindir, "qemu-riscv64")
        if not os.path.exists(qemu_bin):
            self.skipTest("qemu-riscv64 not found at {}; skipping".format(qemu_bin))

        sdlfile = os.path.join(test_path, "usermode", "test_multicore.py")
        if not os.path.exists(sdlfile):
            self.skipTest("test_multicore.py not found; skipping")

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "quetz_tests", "multicore_quorum")
        os.makedirs(outdir, exist_ok=True)

        sst_outfile = os.path.join(outdir, "multicore_quorum.out")
        sst_errfile = os.path.join(outdir, "multicore_quorum.err")
        mpifiles    = os.path.join(outdir, "multicore_quorum.testfile")

        os.environ["SST_HOME"] = sst_prefix

        self.run_sst(sdlfile, sst_outfile, sst_errfile,
                     mpi_out_files=mpifiles,
                     set_cwd=outdir,
                     timeout_sec=240)

        # Both vCPUs must have issued at least one read request, proving the
        # halt-quorum logic did not tear down the simulation early.
        with open(sst_outfile, "r") as f:
            output = f.read()
        self.assertIn("read_requests.0", output,
            "vCPU 0 read_requests not found in output")
        self.assertIn("read_requests.1", output,
            "vCPU 1 read_requests not found in output")

        val = stat_sum(output, "read_requests.1")
        self.assertIsNotNone(val, "Could not parse read_requests.1 from output")
        self.assertGreater(val, 0,
            "vCPU 1 read_requests is 0 — halt quorum may have "
            "terminated simulation before core 1 completed")

    # -------------------------------------------------------------------------
    # Class-balance identity: for RISC-V with detailed tracking, the sum of
    # all per-class instruction counters must equal instruction_count.
    # -------------------------------------------------------------------------
    def test_quetz_riscv_class_balance(self):
        test_path = self.get_testsuite_dir()

        sst_prefix, sst_bindir, sst_libexec = sst_paths()

        qemu_bin = os.path.join(sst_bindir, "qemu-riscv64")
        vanadis_hello = os.path.normpath(os.path.join(
            test_path, "../../vanadis/tests/small"
                       "/basic-io/hello-world/riscv64/hello-world"))

        if not os.path.exists(qemu_bin):
            self.skipTest("qemu-riscv64 not found; skipping")
        if not os.path.exists(vanadis_hello):
            self.skipTest("vanadis hello-world not found; skipping")

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "quetz_tests", "class_balance")
        os.makedirs(outdir, exist_ok=True)

        sdlfile     = os.path.join(test_path, "usermode", "basic_quetz.py")
        sst_outfile = os.path.join(outdir, "class_balance.out")
        sst_errfile = os.path.join(outdir, "class_balance.err")
        mpifiles    = os.path.join(outdir, "class_balance.testfile")

        make_usermode_env(sst_prefix, sst_libexec, qemu_bin, vanadis_hello,
                          with_l1=False, isa="", detailed=True)

        self.run_sst(sdlfile, sst_outfile, sst_errfile,
                     mpi_out_files=mpifiles,
                     set_cwd=outdir,
                     timeout_sec=120)

        try:
            assert_class_balance(parse_stats(sst_outfile), core_id=0)
        except AssertionError as e:
            self.fail(str(e))

    # -------------------------------------------------------------------------
    # Cache-line split: x86 hello must produce split_read_requests > 0.
    # -------------------------------------------------------------------------
    def test_quetz_wide_split(self):
        test_path = self.get_testsuite_dir()

        sst_prefix, sst_bindir, sst_libexec = sst_paths()

        qemu_bin = os.path.join(sst_bindir, "qemu-x86_64")
        exe_abs  = os.path.normpath(os.path.join(test_path, "binaries", "hello_x86_64"))

        if not os.path.exists(qemu_bin):
            self.skipTest("qemu-x86_64 not found; skipping")
        if not os.path.exists(exe_abs):
            self.skipTest("hello_x86_64 binary not found; skipping")

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "quetz_tests", "wide_split")
        os.makedirs(outdir, exist_ok=True)

        sdlfile     = os.path.join(test_path, "usermode", "test_wide_split.py")
        sst_outfile = os.path.join(outdir, "wide_split.out")
        sst_errfile = os.path.join(outdir, "wide_split.err")
        mpifiles    = os.path.join(outdir, "wide_split.testfile")

        os.environ["SST_HOME"] = sst_prefix

        self.run_sst(sdlfile, sst_outfile, sst_errfile,
                     mpi_out_files=mpifiles,
                     set_cwd=outdir,
                     timeout_sec=120)

        with open(sst_outfile, "r") as f:
            output = f.read()

        val = stat_sum(output, "split_read_requests.0")
        self.assertIsNotNone(val,
            "Could not parse split_read_requests.0 from output")
        self.assertGreater(val, 0,
            "split_read_requests.0 is 0 — wide-access line split "
            "loop may not be exercised")

    # -------------------------------------------------------------------------
    def test_quetz_aarch64_class_balance(self):
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec = sst_paths()

        qemu_bin = os.path.join(sst_bindir, "qemu-aarch64")
        exe_abs  = os.path.normpath(os.path.join(test_path, "binaries", "hello_aarch64"))

        if not os.path.exists(qemu_bin):
            self.skipTest("qemu-aarch64 not found; skipping")
        if not os.path.exists(exe_abs):
            self.skipTest("hello_aarch64 not found; skipping")

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "quetz_tests", "aarch64_class_balance")
        os.makedirs(outdir, exist_ok=True)

        sdlfile     = os.path.join(test_path, "usermode", "basic_quetz.py")
        sst_outfile = os.path.join(outdir, "aarch64_class_balance.out")
        sst_errfile = os.path.join(outdir, "aarch64_class_balance.err")
        mpifiles    = os.path.join(outdir, "aarch64_class_balance.testfile")

        make_usermode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                          with_l1=False, isa="aarch64", detailed=True)

        self.run_sst(sdlfile, sst_outfile, sst_errfile,
                     mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=120)

        try:
            assert_class_balance(parse_stats(sst_outfile), core_id=0)
        except AssertionError as e:
            self.fail(str(e))

    # -------------------------------------------------------------------------
    def test_quetz_latency_floor(self):
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec = sst_paths()

        qemu_bin = os.path.join(sst_bindir, "qemu-riscv64")
        vanadis_hello = os.path.normpath(os.path.join(
            test_path, "../../vanadis/tests/small"
                       "/basic-io/hello-world/riscv64/hello-world"))

        if not os.path.exists(qemu_bin):
            self.skipTest("qemu-riscv64 not found; skipping")
        if not os.path.exists(vanadis_hello):
            self.skipTest("vanadis hello-world not found; skipping")

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "quetz_tests", "latency_floor")
        os.makedirs(outdir, exist_ok=True)

        sdlfile     = os.path.join(test_path, "usermode", "basic_quetz.py")
        sst_outfile = os.path.join(outdir, "latency_floor.out")
        sst_errfile = os.path.join(outdir, "latency_floor.err")
        mpifiles    = os.path.join(outdir, "latency_floor.testfile")

        make_usermode_env(sst_prefix, sst_libexec, qemu_bin, vanadis_hello,
                          with_l1=False, detailed=True)

        self.run_sst(sdlfile, sst_outfile, sst_errfile,
                     mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=120)

        stats = parse_stats(sst_outfile)
        reads = stats.get("cpu.read_requests.0", 0)
        lat   = stats.get("cpu.read_latency.0", 0)
        self.assertGreater(reads, 0, "no read_requests recorded")
        if reads > 0:
            self.assertGreaterEqual(lat / reads, 100,
                "read_latency per request below 100ns backend access_time")

    # -------------------------------------------------------------------------
    def test_quetz_determinism(self):
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec = sst_paths()

        qemu_bin = os.path.join(sst_bindir, "qemu-riscv64")
        vanadis_hello = os.path.normpath(os.path.join(
            test_path, "../../vanadis/tests/small"
                       "/basic-io/hello-world/riscv64/hello-world"))

        if not os.path.exists(qemu_bin) or not os.path.exists(vanadis_hello):
            self.skipTest("riscv64 hello prerequisites missing; skipping")

        sdlfile = os.path.join(test_path, "usermode", "basic_quetz.py")
        outdir  = os.path.join(self.get_test_output_run_dir(),
                               "quetz_tests", "determinism")
        os.makedirs(outdir, exist_ok=True)

        make_usermode_env(sst_prefix, sst_libexec, qemu_bin, vanadis_hello,
                          with_l1=False, detailed=True)

        lines_a = lines_b = None
        for run_id, label in enumerate(("a", "b")):
            out = os.path.join(outdir, "determinism_{}.out".format(label))
            err = os.path.join(outdir, "determinism_{}.err".format(label))
            mpi = os.path.join(outdir, "determinism_{}.testfile".format(label))
            self.run_sst(sdlfile, out, err, mpi_out_files=mpi,
                         set_cwd=outdir, timeout_sec=120)
            if run_id == 0:
                lines_a = filtered_stat_lines(out)
            else:
                lines_b = filtered_stat_lines(out)

        self.assertEqual(lines_a, lines_b,
            "Filtered stat lines differ between back-to-back runs")

    # -------------------------------------------------------------------------
    def test_quetz_stride_scaling(self):
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec = sst_paths()

        qemu_bin = os.path.join(sst_bindir, "qemu-riscv64")
        if not os.path.exists(qemu_bin):
            self.skipTest("qemu-riscv64 not found; skipping")

        sdlfile = os.path.join(test_path, "usermode", "basic_quetz.py")
        outdir  = os.path.join(self.get_test_output_run_dir(),
                               "quetz_tests", "stride_scaling")
        os.makedirs(outdir, exist_ok=True)

        reads_by_stride = {}
        for stride, exe_name in ((1, "stride_read_1"), (64, "stride_read_64")):
            exe_abs = os.path.normpath(os.path.join(test_path, "binaries", exe_name))
            if not os.path.exists(exe_abs):
                self.skipTest("{} not built; run build_microbench.sh".format(exe_name))

            out = os.path.join(outdir, "{}.out".format(exe_name))
            err = os.path.join(outdir, "{}.err".format(exe_name))
            mpi = os.path.join(outdir, "{}.testfile".format(exe_name))
            make_usermode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                              with_l1=True, detailed=False)
            self.run_sst(sdlfile, out, err, mpi_out_files=mpi,
                         set_cwd=outdir, timeout_sec=180)
            reads_by_stride[stride] = parse_stats(out).get("cpu.read_requests.0", 0)

        self.assertGreater(reads_by_stride[1], reads_by_stride[64],
            "stride-1 should issue more read_requests than stride-64 through L1")

    # -------------------------------------------------------------------------
    def test_quetz_config_negative(self):
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec = sst_paths()

        qemu_bin = os.path.join(sst_bindir, "qemu-riscv64")
        vanadis_hello = os.path.normpath(os.path.join(
            test_path, "../../vanadis/tests/small"
                       "/basic-io/hello-world/riscv64/hello-world"))

        if not os.path.exists(qemu_bin) or not os.path.exists(vanadis_hello):
            self.skipTest("riscv64 hello prerequisites missing; skipping")

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "quetz_tests", "config_negative")
        os.makedirs(outdir, exist_ok=True)

        sdlfile     = os.path.join(test_path, "usermode", "test_config_bad.py")
        sst_outfile = os.path.join(outdir, "config_negative.out")
        sst_errfile = os.path.join(outdir, "config_negative.err")
        mpifiles    = os.path.join(outdir, "config_negative.testfile")

        os.environ["QUETZ_EXE"]  = vanadis_hello
        os.environ["QUETZ_QEMU"] = qemu_bin
        os.environ["SST_HOME"]   = sst_prefix

        self.run_sst(sdlfile, sst_outfile, sst_errfile,
                     mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=60)

        err_text = ""
        if os.path.exists(sst_errfile):
            with open(sst_errfile, "r") as f:
                err_text = f.read()
        if os.path.exists(sst_outfile):
            with open(sst_outfile, "r") as f:
                err_text += f.read()

        self.assertIn("detailed_instruction_tracking", err_text,
            "Expected fatal about compute_latency without detailed tracking")


# ---------------------------------------------------------------------------
class testcase_quetz_sysmode(SSTTestCase):

    def setUp(self):
        super(type(self), self).setUp()

    def tearDown(self):
        super(type(self), self).tearDown()

    # -------------------------------------------------------------------------
    @parameterized.expand(quetz_sysmode_matrix, name_func=gen_custom_name)
    def test_quetz_sysmode(self, testnum, testname, qemu_target, exe_rel,
                           qemu_args, loader, ram_start, ram_end,
                           memmaps, uart_echo_input, timeout_sec):
        log_debug("Quetz sysmode test #{} ({}): qemu={}".format(
            testnum, testname, qemu_target))
        self._sysmode_test_template(
            testnum, testname, qemu_target, exe_rel,
            qemu_args, loader, ram_start, ram_end,
            memmaps, uart_echo_input, timeout_sec)

    # -------------------------------------------------------------------------
    def _sysmode_test_template(self, testnum, testname, qemu_target, exe_rel,
                                qemu_args, loader, ram_start, ram_end,
                                memmaps, uart_echo_input, testtimeout=120):
        test_path = self.get_testsuite_dir()   # .../quetz/tests/

        sst_prefix, sst_bindir, sst_libexec = sst_paths()

        # For x86 tests, check system PATH too.
        import shutil
        qemu_bin = os.path.join(sst_bindir, qemu_target)
        if not os.path.exists(qemu_bin):
            found = shutil.which(qemu_target)
            if found:
                qemu_bin = found

        exe_abs = os.path.normpath(os.path.join(test_path, exe_rel))

        if not os.path.exists(qemu_bin):
            self.skipTest("{} not found; skipping".format(qemu_target))
        if not os.path.exists(exe_abs):
            self.skipTest("firmware not found at {}; skipping".format(exe_abs))

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "quetz_sysmode_tests", testname)
        os.makedirs(outdir, exist_ok=True)

        sdlfile     = os.path.join(test_path, "sysmode", "basic_quetz_sysmode.py")
        test_label  = "test_quetz_sysmode_{}".format(testname)
        sst_outfile = os.path.join(outdir, test_label + ".out")
        sst_errfile = os.path.join(outdir, test_label + ".err")
        mpifiles    = os.path.join(outdir, test_label + ".testfile")
        ref_outfile = os.path.join(test_path, "sysmode", "small",
                                   testname, "sst.stdout.gold")

        # Write UART echo input to a temp file if needed.
        stdin_file = ""
        if uart_echo_input is not None:
            stdin_path = os.path.join(outdir, "uart_stdin.bin")
            with open(stdin_path, "wb") as f:
                f.write(uart_echo_input)
            stdin_file = stdin_path

        make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                         qemu_args, loader, ram_start, ram_end, memmaps,
                         stdin_file=stdin_file)

        self.run_sst(sdlfile, sst_outfile, sst_errfile,
                     mpi_out_files=mpifiles,
                     set_cwd=outdir,
                     timeout_sec=testtimeout)

        if os.path.exists(ref_outfile) and should_compare_gold():
            cmp_result = compare_gold(testname, sst_outfile, ref_outfile,
                                      update_files=updateFiles)
            self.assertTrue(cmp_result,
                "Quetz sysmode output {} does not match reference {}".format(
                    sst_outfile, ref_outfile))
        elif os.path.exists(ref_outfile):
            log_testing_note(
                "Quetz sysmode test {} gold skipped (QUETZ_SKIP_GOLD=1)".format(
                    testname))
        else:
            log_testing_note(
                "Quetz sysmode test {} has no gold file; did not compare".format(
                    testname))

        # Positive UART capture check: when the test injects stdin bytes for
        # UART echo, verify the captured UART output appears in SST's stdout.
        # The QuetzStatsFilter strips non-stat lines so this is not covered by
        # the gold-file comparison above.
        if uart_echo_input is not None and os.path.exists(sst_outfile):
            with open(sst_outfile, "r") as f:
                raw_output = f.read()
            self.assertIn("UART[0]:", raw_output,
                "Sysmode UART echo test '{}' did not produce UART[0]: output "
                "— store-data capture may not be working (requires QEMU 9.0+ "
                "with qemu_plugin_mem_get_value)".format(testname))

    # -------------------------------------------------------------------------
    # QuetzConfigManager platform-preset coverage: run the same firmware as
    # the riscv64_virt_hello sysmode test, but supply only platform= and let
    # the C++ preset register supply qemu_args, loader, and region handlers.
    # Reuses the riscv64_virt_hello gold file so any drift would surface as
    # a stat mismatch.
    def test_quetz_sysmode_preset_riscv64_virt(self):
        testname    = "preset_riscv64_virt"
        gold_test   = "riscv64_virt_hello"
        qemu_target = "qemu-system-riscv64"
        platform    = "riscv64_virt"
        exe_rel     = "sysmode/firmware/riscv_virt_hello"

        test_path   = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec = sst_paths()

        import shutil
        qemu_bin = os.path.join(sst_bindir, qemu_target)
        if not os.path.exists(qemu_bin):
            found = shutil.which(qemu_target)
            if found:
                qemu_bin = found
        exe_abs = os.path.normpath(os.path.join(test_path, exe_rel))

        if not os.path.exists(qemu_bin):
            self.skipTest("{} not found; skipping".format(qemu_target))
        if not os.path.exists(exe_abs):
            self.skipTest("firmware not found at {}; skipping".format(exe_abs))

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "quetz_sysmode_tests", testname)
        os.makedirs(outdir, exist_ok=True)

        sdlfile     = os.path.join(test_path, "sysmode", "preset_quetz_sysmode.py")
        test_label  = "test_quetz_sysmode_{}".format(testname)
        sst_outfile = os.path.join(outdir, test_label + ".out")
        sst_errfile = os.path.join(outdir, test_label + ".err")
        mpifiles    = os.path.join(outdir, test_label + ".testfile")
        ref_outfile = os.path.join(test_path, "sysmode", "small",
                                   gold_test, "sst.stdout.gold")

        make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                         "", "-kernel", 0, 0xFFFFFFFF, [],
                         platform=platform)

        self.run_sst(sdlfile, sst_outfile, sst_errfile,
                     mpi_out_files=mpifiles,
                     set_cwd=outdir,
                     timeout_sec=120)

        if os.path.exists(ref_outfile) and should_compare_gold():
            cmp_result = compare_gold(testname, sst_outfile, ref_outfile,
                                      update_files=updateFiles)
            self.assertTrue(cmp_result,
                "Quetz preset sysmode output {} does not match reference {} "
                "(platform preset should yield equivalent stats to explicit "
                "region handler params)".format(sst_outfile, ref_outfile))
        elif os.path.exists(ref_outfile):
            log_testing_note(
                "Quetz preset sysmode test {} gold skipped (QUETZ_SKIP_GOLD=1)".format(
                    testname))
        else:
            log_testing_note(
                "No gold file at {}; preset test ran but was not compared".format(
                    ref_outfile))

    # -------------------------------------------------------------------------
    def test_quetz_sysmode_filtered_only(self):
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec = sst_paths()

        qemu_target = "qemu-system-riscv64"
        exe_rel     = "sysmode/firmware/riscv_virt_hello"
        import shutil
        qemu_bin = os.path.join(sst_bindir, qemu_target)
        if not os.path.exists(qemu_bin):
            found = shutil.which(qemu_target)
            if found:
                qemu_bin = found
        exe_abs = os.path.normpath(os.path.join(test_path, exe_rel))

        if not os.path.exists(qemu_bin) or not os.path.exists(exe_abs):
            self.skipTest("sysmode prerequisites missing; skipping")

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "quetz_sysmode_tests", "filtered_only")
        os.makedirs(outdir, exist_ok=True)

        sdlfile     = os.path.join(test_path, "sysmode", "basic_quetz_sysmode.py")
        sst_outfile = os.path.join(outdir, "filtered_only.out")
        sst_errfile = os.path.join(outdir, "filtered_only.err")
        mpifiles    = os.path.join(outdir, "filtered_only.testfile")

        memmaps = [("all", 0, 0xFFFFFFFF, "filtered")]
        make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                         "-machine virt -nographic -bios none",
                         "-kernel", 0, 0xFFFFFFFF, memmaps)

        self.run_sst(sdlfile, sst_outfile, sst_errfile,
                     mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=120)

        stats = parse_stats(sst_outfile)
        fwd_reads  = stats.get("cpu.read_requests.0", 0)
        fwd_writes = stats.get("cpu.write_requests.0", 0)
        filt_reads = stats.get("cpu.filtered_reads.0", 0)
        filt_writes = stats.get("cpu.filtered_writes.0", 0)

        self.assertEqual(fwd_reads + fwd_writes, 0,
            "filtered-only sysmode should not forward traffic to memHierarchy")
        self.assertGreater(filt_reads + filt_writes, 0,
            "filtered-only sysmode should record filtered MMIO/RAM ops")

    # -------------------------------------------------------------------------
    def test_quetz_sysmode_mmio_basic(self):
        """MMIO write and read must use mmio_link, not cache_link."""
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec = sst_paths()

        qemu_target = "qemu-system-riscv64"
        exe_rel     = "sysmode/firmware/riscv_virt_mmio_poke"
        import shutil
        qemu_bin = os.path.join(sst_bindir, qemu_target)
        if not os.path.exists(qemu_bin):
            found = shutil.which(qemu_target)
            if found:
                qemu_bin = found
        exe_abs = os.path.normpath(os.path.join(test_path, exe_rel))

        if not os.path.exists(qemu_bin):
            self.skipTest("{} not found; skipping".format(qemu_target))
        if not os.path.exists(exe_abs):
            self.skipTest("mmio poke firmware not found at {}; "
                          "run sysmode/firmware/build.sh".format(exe_abs))

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "quetz_sysmode_tests", "mmio_basic")
        os.makedirs(outdir, exist_ok=True)

        sdlfile     = os.path.join(test_path, "sysmode", "basic_quetz_mmio.py")
        sst_outfile = os.path.join(outdir, "mmio_basic.out")
        sst_errfile = os.path.join(outdir, "mmio_basic.err")
        mpifiles    = os.path.join(outdir, "mmio_basic.testfile")

        memmaps = [
            ("kernel_dram", 0x80000000, 0x800FFFFF, "filtered"),
            ("sub_ram",     0x00000000, 0x7FFFFFFF, "filtered"),
        ]
        make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                         "-machine virt -nographic -bios none",
                         "-kernel", 0, 0xFFFFFFFF, memmaps)
        os.environ["QUETZ_MMIO_START"] = "0x80100000"
        os.environ["QUETZ_MMIO_END"]   = "0x801003FF"
        enable_mmio_payload_delivery()

        self.run_sst(sdlfile, sst_outfile, sst_errfile,
                     mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=120)

        stats = parse_stats(sst_outfile)
        mmio_writes = stats.get("cpu.mmio_write_requests.0", 0)
        mmio_reads  = stats.get("cpu.mmio_read_requests.0", 0)
        cache_writes = stats.get("cpu.write_requests.0", 0)
        cache_reads  = stats.get("cpu.read_requests.0", 0)

        self.assertGreaterEqual(mmio_writes, 1,
            "MMIO write should appear on mmio_write_requests")
        self.assertGreaterEqual(mmio_reads, 1,
            "MMIO read should appear on mmio_read_requests (mmioEx returns 9 for write 3)")
        self.assertEqual(cache_writes, 0,
            "MMIO poke firmware should not forward writes on cache_link "
            "(UART/testdev are filtered; MMIO uses mmio_link)")
        self.assertEqual(cache_reads, 0,
            "MMIO poke firmware should not forward reads on cache_link")

    # -------------------------------------------------------------------------
    def test_quetz_sysmode_gpu_kernel(self):
        """QuetzGpuDevice: doorbell launches kernels with timed BUSY/IDLE."""
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec = sst_paths()

        qemu_target = "qemu-system-riscv64"
        exe_rel     = "sysmode/firmware/riscv_virt_gpu_kernel"
        import shutil
        qemu_bin = os.path.join(sst_bindir, qemu_target)
        if not os.path.exists(qemu_bin):
            found = shutil.which(qemu_target)
            if found:
                qemu_bin = found
        exe_abs = os.path.normpath(os.path.join(test_path, exe_rel))

        if not os.path.exists(qemu_bin):
            self.skipTest("{} not found; skipping".format(qemu_target))
        if not os.path.exists(exe_abs):
            self.skipTest("gpu kernel firmware not found at {}; "
                          "run sysmode/firmware/build.sh".format(exe_abs))

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "quetz_sysmode_tests", "gpu_kernel")
        os.makedirs(outdir, exist_ok=True)

        sdlfile     = os.path.join(test_path, "sysmode", "basic_quetz_gpu.py")
        sst_outfile = os.path.join(outdir, "gpu_kernel.out")
        sst_errfile = os.path.join(outdir, "gpu_kernel.err")
        mpifiles    = os.path.join(outdir, "gpu_kernel.testfile")

        # Kernel is linked at 0x80000000 (link_rv64.ld); filter it so stack/text
        # traffic does not count as cache_link writes alongside MMIO doorbells.
        memmaps = [
            ("kernel_dram", 0x80000000, 0x800FFFFF, "filtered"),
            ("sub_ram",     0x00000000, 0x7FFFFFFF, "filtered"),
        ]
        make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                         "-machine virt -nographic -bios none",
                         "-kernel", 0, 0xFFFFFFFF, memmaps)
        os.environ["QUETZ_MMIO_START"] = "0x80100000"
        os.environ["QUETZ_MMIO_END"]   = "0x801003FF"
        enable_mmio_payload_delivery()

        self.run_sst(sdlfile, sst_outfile, sst_errfile,
                     mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=180)

        stats = parse_stats(sst_outfile)
        with open(sst_outfile, "r") as f:
            raw_output = f.read()

        mmio_writes = stats.get("cpu.mmio_write_requests.0", 0)
        mmio_reads  = stats.get("cpu.mmio_read_requests.0", 0)
        cache_writes = stats.get("cpu.write_requests.0", 0)

        kernels_launched = stat_sum(raw_output, "gpu.kernels_launched")
        busy_cycles = stat_sum(raw_output, "gpu.busy_cycles")
        doorbell_while_busy = stat_sum(raw_output, "gpu.doorbell_while_busy")

        self.assertGreaterEqual(mmio_writes, 6,
            "expected doorbell + latency_override MMIO writes")
        self.assertGreater(mmio_reads, 4,
            "STATUS spin should issue one mmio read per poll iteration")
        self.assertEqual(cache_writes, 0,
            "GPU doorbell must not escape to cache_link")
        self.assertIsNotNone(kernels_launched,
            "gpu.kernels_launched not found in output")
        self.assertEqual(kernels_launched, 4,
            "firmware launches four kernels (incl. LATENCY_OVERRIDE=0 fallback)")
        self.assertIsNotNone(busy_cycles,
            "gpu.busy_cycles not found in output")
        default_kernel_latency = 5000
        expected_busy_min = 1000 + 5000 + 20000 + default_kernel_latency
        self.assertGreaterEqual(busy_cycles, expected_busy_min - 4,
            "gpu.busy_cycles should reflect firmware latencies plus default fallback")
        self.assertIsNotNone(doorbell_while_busy,
            "gpu.doorbell_while_busy not found in output")
        self.assertEqual(doorbell_while_busy, 0,
            "guest STATUS spin must see real payloads before each doorbell")

    # -------------------------------------------------------------------------
    def test_quetz_sysmode_gpu_fft(self):
        """256-pt FFT computed on the guest CPU, TIMED by the synthetic GPU.

        No balar / no GPGPU-Sim: the QuetzGpuDevice is a pure latency model, so
        the FFT math runs on the rv64gc core and each of the 11 "kernels" (1 H2D +
        1 bitrev + 8 stages + 1 D2H, mirroring the balar FFT call structure) is
        timed by a doorbell. Impulse input -> X[k]=1+0j for all k, verified
        bit-exactly (correct_words=512/512). Runs anywhere qemu-system-riscv64
        exists (no GPGPUSIM_ROOT / .cfg / cuda guards)."""
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec = sst_paths()

        qemu_target = "qemu-system-riscv64"
        exe_rel     = "sysmode/firmware/riscv_virt_gpu_fft"
        import shutil
        qemu_bin = os.path.join(sst_bindir, qemu_target)
        if not os.path.exists(qemu_bin):
            found = shutil.which(qemu_target)
            if found:
                qemu_bin = found
        exe_abs = os.path.normpath(os.path.join(test_path, exe_rel))

        if not os.path.exists(qemu_bin):
            self.skipTest("{} not found; skipping".format(qemu_target))
        if not os.path.exists(exe_abs):
            self.skipTest("gpu fft firmware not found at {}; "
                          "run sysmode/firmware/build.sh".format(exe_abs))

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "quetz_sysmode_tests", "gpu_fft")
        os.makedirs(outdir, exist_ok=True)

        sdlfile     = os.path.join(test_path, "sysmode", "basic_quetz_gpu.py")
        sst_outfile = os.path.join(outdir, "gpu_fft.out")
        sst_errfile = os.path.join(outdir, "gpu_fft.err")
        mpifiles    = os.path.join(outdir, "gpu_fft.testfile")

        # Region-handler map (order matters — first match wins):
        #   uart0 : capture the correct_words= banner (must precede sub_ram, else
        #           UART at 0x10000000 falls into sub_ram and is filtered away)
        #   kernel_dram / sub_ram : guest RAM is FILTERED — serviced by QEMU and
        #     consumed locally in SST (the normal Quetz sysmode path). The FFT
        #     compute runs in QEMU's memory; only the doorbells/UART are modeled.
        #     FFT arrays + stack live below 0x80100000; the GPU MMIO window
        #     (slot-0 MmioForwardRegionHandler) owns 0x80100000+.
        # (The firmware enables mstatus.FS in _start so its float ops don't trap.)
        memmaps = [
            ("uart0",       0x10000000, 0x10000FFF, "uart"),
            ("kernel_dram", 0x80000000, 0x800FFFFF, "filtered"),
            ("sub_ram",     0x00000000, 0x7FFFFFFF, "filtered"),
        ]
        make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                         "-machine virt -nographic -bios none",
                         "-kernel", 0, 0xFFFFFFFF, memmaps)
        os.environ["QUETZ_MMIO_START"] = "0x80100000"
        os.environ["QUETZ_MMIO_END"]   = "0x801003FF"
        enable_mmio_payload_delivery()

        self.run_sst(sdlfile, sst_outfile, sst_errfile,
                     mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=180)

        stats = parse_stats(sst_outfile)
        with open(sst_outfile, "r") as f:
            raw_output = f.read()

        # Bit-exact FFT: impulse -> 1+0j everywhere (512 float words for N=256).
        self.assertIn("GPU FFT (synthetic) correct_words=512/512", raw_output,
            "CPU FFT not bit-exact through the synthetic-GPU timed run")

        mmio_writes = stats.get("cpu.mmio_write_requests.0", 0)
        kernels_launched = stat_sum(raw_output, "gpu.kernels_launched")
        busy_cycles      = stat_sum(raw_output, "gpu.busy_cycles")
        doorbell_while_busy = stat_sum(raw_output, "gpu.doorbell_while_busy")

        # 11 timed kernels => >= 22 mmio writes (latency-override + doorbell each).
        self.assertGreaterEqual(mmio_writes, 2 * 11,
            "11 timed kernels => >= 22 mmio writes (latency-override + doorbell)")
        self.assertIsNotNone(kernels_launched,
            "gpu.kernels_launched not found in output")
        self.assertEqual(kernels_launched, 11,
            "FFT issues 11 timed kernels (1 H2D + 1 bitrev + 8 stages + 1 D2H)")
        self.assertIsNotNone(busy_cycles, "gpu.busy_cycles not found in output")
        # Each launch overrides latency to 5000 cycles; 11 * 5000 minus small slack.
        self.assertGreaterEqual(busy_cycles, 11 * 5000 - 11,
            "gpu.busy_cycles should reflect 11 timed 5000-cycle kernels")
        self.assertIsNotNone(doorbell_while_busy,
            "gpu.doorbell_while_busy not found in output")
        self.assertEqual(doorbell_while_busy, 0,
            "synchronous timed launches: each doorbell waits out the prior kernel")

    # -------------------------------------------------------------------------
    def test_quetz_sysmode_gpu_fft_offload(self):
        """256-pt FFT computed ON THE DEVICE (kernel_type=fft), not the guest CPU.

        The contrast to test_quetz_sysmode_gpu_fft: there the guest runs the
        butterflies and the device only counts latency; here QuetzGpuDevice
        DMA-reads the input from the SST-backed window (0x90000000, trapped by a
        second sst-mmio-bridge aperture), computes the FFT in C++, stays BUSY for
        coeff*N*log2(N) cycles, and DMA-writes the result back. The guest only
        fills the input, programs 3 registers, rings one doorbell, and verifies.
        Headline assertions: ONE kernel, modeled busy time, and a guest
        instruction count far too small to have computed the FFT itself."""
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec = sst_paths()

        qemu_target = "qemu-system-riscv64"
        exe_rel     = "sysmode/firmware/riscv_virt_gpu_fft_offload"
        import shutil
        qemu_bin = os.path.join(sst_bindir, qemu_target)
        if not os.path.exists(qemu_bin):
            found = shutil.which(qemu_target)
            if found:
                qemu_bin = found
        exe_abs = os.path.normpath(os.path.join(test_path, exe_rel))

        if not os.path.exists(qemu_bin):
            self.skipTest("{} not found; skipping".format(qemu_target))
        if not os.path.exists(exe_abs):
            self.skipTest("gpu fft offload firmware not found at {}; "
                          "run sysmode/firmware/build.sh".format(exe_abs))

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "quetz_sysmode_tests", "gpu_fft_offload")
        os.makedirs(outdir, exist_ok=True)

        sdlfile     = os.path.join(test_path, "sysmode", "basic_quetz_gpu_compute.py")
        sst_outfile = os.path.join(outdir, "gpu_fft_offload.out")
        sst_errfile = os.path.join(outdir, "gpu_fft_offload.err")
        mpifiles    = os.path.join(outdir, "gpu_fft_offload.testfile")

        # The SDL installs its own region handlers (the FFT window needs none —
        # window accesses arrive via the sync mailbox, not the trace path).
        make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                         "-machine virt -nographic -bios none",
                         "-kernel", 0, 0xFFFFFFFF, [])
        os.environ["QUETZ_MMIO_START"] = "0x80100000"
        os.environ["QUETZ_MMIO_END"]   = "0x801003FF"
        # SST-backed buffer window: the launcher creates the second bridge
        # aperture from these (without them the guest faults on its first store).
        os.environ["QUETZ_SST_WIN_START"] = "0x90000000"
        os.environ["QUETZ_SST_WIN_END"]   = "0x9000FFFF"
        enable_mmio_payload_delivery()

        try:
            self.run_sst(sdlfile, sst_outfile, sst_errfile,
                         mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=180)
        finally:
            os.environ.pop("QUETZ_SST_WIN_START", None)
            os.environ.pop("QUETZ_SST_WIN_END", None)

        stats = parse_stats(sst_outfile)
        with open(sst_outfile, "r") as f:
            raw_output = f.read()

        # Bit-exact device FFT: impulse -> 1+0j everywhere, verified by the guest
        # reading the DMA-written result back out of the SST window.
        self.assertIn("GPU FFT offload correct_words=512/512", raw_output,
            "device-computed FFT result not bit-exact through DMA writeback")

        kernels_launched    = stat_sum(raw_output, "gpu.kernels_launched")
        busy_cycles         = stat_sum(raw_output, "gpu.busy_cycles")
        doorbell_while_busy = stat_sum(raw_output, "gpu.doorbell_while_busy")
        mmio_writes = stats.get("cpu.mmio_write_requests.0", 0)
        mmio_reads  = stats.get("cpu.mmio_read_requests.0", 0)
        insn_count  = stats.get("cpu.instruction_count.0", 0)

        self.assertEqual(kernels_launched, 1,
            "device FFT is ONE kernel (no per-stage doorbells)")
        # Modeled compute: coeff(20) * N(256) * log2N(8) = 40960 cycles.
        self.assertGreaterEqual(busy_cycles, 20 * 256 * 8,
            "gpu.busy_cycles should reflect coeff*N*log2(N) modeled compute")
        self.assertEqual(doorbell_while_busy, 0,
            "blocking doorbell: guest cannot re-ring while the op is in flight")
        # 512 input-fill stores + 3 setup registers + 1 doorbell.
        self.assertGreaterEqual(mmio_writes, 512 + 3 + 1,
            "guest must fill the input buffer through the SST window")
        # 512 verify loads (+ status poll).
        self.assertGreaterEqual(mmio_reads, 512,
            "guest must read the DMA-written result back from the SST window")
        # The headline: the guest issued no butterflies. The CPU-compute variant
        # executes hundreds of thousands of instructions; the offload guest only
        # fills/verifies buffers. Generous bound to absorb QEMU boot noise.
        self.assertLess(insn_count, 50000,
            "guest instruction count too high — did the CPU compute the FFT?")

    # -------------------------------------------------------------------------
    def test_quetz_sysmode_gpu_fft_coldfire(self):
        """ColdFire (m68k) 256-pt FFT computed on the guest CPU, TIMED by the
        synthetic GPU. Same as the RISC-V gpu_fft test but for the 32-bit
        big-endian NXP mcf5208evb — the FPU-less ColdFire computes the FFT in
        Q16.16 fixed point (the m68k libgcc soft-float helpers hang on ColdFire),
        and the synthetic QuetzGpuDevice times the same 11 kernels. Bit-exact
        (correct_words=512/512), balar-free (no GPGPUSIM_ROOT / .cfg / cuda)."""
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec = sst_paths()

        qemu_target = "qemu-system-m68k"
        exe_rel     = "sysmode/firmware/coldfire_gpu_fft"
        import shutil
        qemu_bin = os.path.join(sst_bindir, qemu_target)
        if not os.path.exists(qemu_bin):
            found = shutil.which(qemu_target)
            if found:
                qemu_bin = found
        exe_abs = os.path.normpath(os.path.join(test_path, exe_rel))

        if not os.path.exists(qemu_bin):
            self.skipTest("{} not found; rebuild QEMU with m68k-softmmu".format(qemu_target))
        if not os.path.exists(exe_abs):
            self.skipTest("coldfire_gpu_fft not found at {}; "
                          "run M68K_CC=m68k-linux-gnu-gcc ./build.sh".format(exe_abs))

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "quetz_sysmode_tests", "gpu_fft_coldfire")
        os.makedirs(outdir, exist_ok=True)

        sdlfile     = os.path.join(test_path, "sysmode", "basic_quetz_gpu_coldfire.py")
        sst_outfile = os.path.join(outdir, "gpu_fft_coldfire.out")
        sst_errfile = os.path.join(outdir, "gpu_fft_coldfire.err")
        mpifiles    = os.path.join(outdir, "gpu_fft_coldfire.testfile")

        # ColdFire mcf5208evb: SDRAM 0x40000000 (filtered guest RAM), synthetic GPU
        # at 0x70000000, UART 0xfc060000, TestFinisher sentinel 0x80000000. The SDL
        # installs those handlers; make_sysmode_env just sets exe/qemu/args.
        make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                         "-machine mcf5208evb -display none -serial stdio -m 128M",
                         "-kernel", 0x40000000, 0x47FFFFFF, [])
        os.environ["QUETZ_MMIO_START"] = "0x70000000"
        os.environ["QUETZ_MMIO_END"]   = "0x700003FF"
        os.environ.pop("QUETZ_PLATFORM", None)
        enable_mmio_payload_delivery()

        self.run_sst(sdlfile, sst_outfile, sst_errfile,
                     mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=180)

        raw = ""
        if os.path.exists(sst_outfile):
            with open(sst_outfile, "r") as f:
                raw = f.read()
        self.assertNotIn("FATAL", raw)

        self.assertIn("GPU FFT (synthetic) correct_words=512/512", raw,
            "ColdFire fixed-point FFT not bit-exact through the synthetic-GPU run")
        self.assertIn("TESTFINISH[0]", raw,
            "TestFinisher sentinel not triggered; sim may have hung")

        stats = parse_stats(sst_outfile)
        mmio_writes = stats.get("cpu.mmio_write_requests.0", 0)
        kernels_launched = stat_sum(raw, "gpu.kernels_launched")
        busy_cycles      = stat_sum(raw, "gpu.busy_cycles")
        doorbell_while_busy = stat_sum(raw, "gpu.doorbell_while_busy")

        self.assertGreaterEqual(mmio_writes, 2 * 11,
            "11 timed kernels => >= 22 mmio writes (latency-override + doorbell)")
        self.assertEqual(kernels_launched, 11,
            "FFT issues 11 timed kernels (1 H2D + 1 bitrev + 8 stages + 1 D2H)")
        self.assertIsNotNone(busy_cycles, "gpu.busy_cycles not found in output")
        self.assertGreaterEqual(busy_cycles, 11 * 5000 - 11,
            "gpu.busy_cycles should reflect 11 timed 5000-cycle kernels")
        self.assertEqual(doorbell_while_busy, 0,
            "synchronous timed launches: each doorbell waits out the prior kernel")

    # -------------------------------------------------------------------------
    def test_quetz_sysmode_gpu_fft_offload_coldfire(self):
        """ColdFire (m68k) 256-pt FFT computed ON THE DEVICE (kernel_type=fft).

        The big-endian counterpart of test_quetz_sysmode_gpu_fft_offload — and
        the proof that the sync-MMIO window needs no guest byte-swapping: the
        bridge mailbox carries values, SST serializes them little-endian into
        the window, so the FPU-less ColdFire just stores/compares raw IEEE bit
        patterns (0x3F800000 == 1.0f). No soft-float, no Q16.16, no butterflies
        on the guest."""
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec = sst_paths()

        qemu_target = "qemu-system-m68k"
        exe_rel     = "sysmode/firmware/coldfire_gpu_fft_offload"
        import shutil
        qemu_bin = os.path.join(sst_bindir, qemu_target)
        if not os.path.exists(qemu_bin):
            found = shutil.which(qemu_target)
            if found:
                qemu_bin = found
        exe_abs = os.path.normpath(os.path.join(test_path, exe_rel))

        if not os.path.exists(qemu_bin):
            self.skipTest("{} not found; rebuild QEMU with m68k-softmmu".format(qemu_target))
        if not os.path.exists(exe_abs):
            self.skipTest("coldfire_gpu_fft_offload not found at {}; "
                          "run M68K_CC=m68k-linux-gnu-gcc ./build.sh".format(exe_abs))

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "quetz_sysmode_tests", "gpu_fft_offload_coldfire")
        os.makedirs(outdir, exist_ok=True)

        sdlfile     = os.path.join(test_path, "sysmode",
                                   "basic_quetz_gpu_compute_coldfire.py")
        sst_outfile = os.path.join(outdir, "gpu_fft_offload_coldfire.out")
        sst_errfile = os.path.join(outdir, "gpu_fft_offload_coldfire.err")
        mpifiles    = os.path.join(outdir, "gpu_fft_offload_coldfire.testfile")

        make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                         "-machine mcf5208evb -display none -serial stdio -m 128M",
                         "-kernel", 0x40000000, 0x47FFFFFF, [])
        os.environ["QUETZ_MMIO_START"] = "0x70000000"
        os.environ["QUETZ_MMIO_END"]   = "0x700003FF"
        os.environ["QUETZ_SST_WIN_START"] = "0x71000000"
        os.environ["QUETZ_SST_WIN_END"]   = "0x7100FFFF"
        os.environ.pop("QUETZ_PLATFORM", None)
        enable_mmio_payload_delivery()

        try:
            self.run_sst(sdlfile, sst_outfile, sst_errfile,
                         mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=180)
        finally:
            os.environ.pop("QUETZ_SST_WIN_START", None)
            os.environ.pop("QUETZ_SST_WIN_END", None)

        raw = ""
        if os.path.exists(sst_outfile):
            with open(sst_outfile, "r") as f:
                raw = f.read()
        self.assertNotIn("FATAL", raw)

        self.assertIn("GPU FFT offload correct_words=512/512", raw,
            "device-computed FFT result not bit-exact on the big-endian guest")
        self.assertIn("TESTFINISH[0]", raw,
            "TestFinisher sentinel not triggered; sim may have hung")

        stats = parse_stats(sst_outfile)
        kernels_launched    = stat_sum(raw, "gpu.kernels_launched")
        busy_cycles         = stat_sum(raw, "gpu.busy_cycles")
        doorbell_while_busy = stat_sum(raw, "gpu.doorbell_while_busy")
        mmio_writes = stats.get("cpu.mmio_write_requests.0", 0)
        mmio_reads  = stats.get("cpu.mmio_read_requests.0", 0)

        self.assertEqual(kernels_launched, 1,
            "device FFT is ONE kernel (no per-stage doorbells)")
        self.assertGreaterEqual(busy_cycles, 20 * 256 * 8,
            "gpu.busy_cycles should reflect coeff*N*log2(N) modeled compute")
        self.assertEqual(doorbell_while_busy, 0,
            "blocking doorbell: guest cannot re-ring while the op is in flight")
        # 256 points * 2 u32 words = 512 fill stores, plus 3 setup regs + doorbell.
        self.assertGreaterEqual(mmio_writes, 512 + 3 + 1,
            "guest must fill the input buffer through the SST window")
        self.assertGreaterEqual(mmio_reads, 512,
            "guest must read the DMA-written result back from the SST window")

    # -------------------------------------------------------------------------
    def test_quetz_sysmode_gpu_async(self):
        """P4 async offload: posted submit + ticket/completed poll overlap.

        The doorbell returns immediately; the guest reads its submission ticket,
        runs real CPU work while the synthetic GPU is busy, then joins by
        polling the completed-ticket counter. Proves CPU/GPU overlap (the second
        kernel is queued *while* the first runs) with no balar dependency."""
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec = sst_paths()

        qemu_target = "qemu-system-riscv64"
        exe_rel     = "sysmode/firmware/riscv_virt_gpu_async"
        import shutil
        qemu_bin = os.path.join(sst_bindir, qemu_target)
        if not os.path.exists(qemu_bin):
            found = shutil.which(qemu_target)
            if found:
                qemu_bin = found
        exe_abs = os.path.normpath(os.path.join(test_path, exe_rel))

        if not os.path.exists(qemu_bin):
            self.skipTest("{} not found; skipping".format(qemu_target))
        if not os.path.exists(exe_abs):
            self.skipTest("gpu async firmware not found at {}; "
                          "run sysmode/firmware/build.sh".format(exe_abs))

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "quetz_sysmode_tests", "gpu_async")
        os.makedirs(outdir, exist_ok=True)

        sdlfile     = os.path.join(test_path, "sysmode", "basic_quetz_gpu.py")
        sst_outfile = os.path.join(outdir, "gpu_async.out")
        sst_errfile = os.path.join(outdir, "gpu_async.err")
        mpifiles    = os.path.join(outdir, "gpu_async.testfile")

        memmaps = [
            ("kernel_dram", 0x80000000, 0x800FFFFF, "filtered"),
            ("sub_ram",     0x00000000, 0x7FFFFFFF, "filtered"),
        ]
        make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                         "-machine virt -nographic -bios none",
                         "-kernel", 0, 0xFFFFFFFF, memmaps)
        os.environ["QUETZ_MMIO_START"] = "0x80100000"
        os.environ["QUETZ_MMIO_END"]   = "0x801003FF"
        enable_mmio_payload_delivery()

        self.run_sst(sdlfile, sst_outfile, sst_errfile,
                     mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=180)

        stats = parse_stats(sst_outfile)
        with open(sst_outfile, "r") as f:
            raw_output = f.read()

        self.assertIn("ASYNC OVERLAP OK", raw_output,
            "firmware did not confirm async overlap (ticket/completed contract)")

        kernels_launched    = stat_sum(raw_output, "gpu.kernels_launched")
        busy_cycles         = stat_sum(raw_output, "gpu.busy_cycles")
        doorbell_while_busy = stat_sum(raw_output, "gpu.doorbell_while_busy")
        mmio_writes = stats.get("cpu.mmio_write_requests.0", 0)
        mmio_reads  = stats.get("cpu.mmio_read_requests.0", 0)

        self.assertEqual(kernels_launched, 2,
            "firmware posts two async kernels")
        # The async signature: the second submit lands while the first kernel is
        # still BUSY — the doorbell returned without blocking for the GPU op.
        self.assertEqual(doorbell_while_busy, 1,
            "second async submit must be accepted while the first kernel runs")
        self.assertIsNotNone(busy_cycles, "gpu.busy_cycles not found")
        self.assertGreaterEqual(busy_cycles, 2 * 40000 - 4,
            "two 40000-cycle kernels should run to completion")
        self.assertEqual(mmio_writes, 4,
            "two submits = two latency-override + two doorbell writes")
        self.assertGreater(mmio_reads, 4,
            "ticket read + completed-counter poll loop issue mmio reads")

    # -------------------------------------------------------------------------
    def test_quetz_sysmode_gpu_async_engine(self):
        """P4 async engine: Quetz-emulated submit aperture + posted balar-style
        doorbell, validated balar-free.

        The guest posts to a Quetz submit aperture; the async engine drains the
        packet, flushes the scratch range, forwards a doorbell to the synthetic
        GPU (in doorbell_blocking mode), and acks the guest. The device holds
        its write response until the kernel retires, so the COMPLETED counter
        the guest polls only advances at true kernel completion — exercising the
        exact code path the balar offload uses, with no balar dependency."""
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec = sst_paths()

        qemu_target = "qemu-system-riscv64"
        exe_rel     = "sysmode/firmware/riscv_virt_gpu_async_engine"
        import shutil
        qemu_bin = os.path.join(sst_bindir, qemu_target)
        if not os.path.exists(qemu_bin):
            found = shutil.which(qemu_target)
            if found:
                qemu_bin = found
        exe_abs = os.path.normpath(os.path.join(test_path, exe_rel))

        if not os.path.exists(qemu_bin):
            self.skipTest("{} not found; skipping".format(qemu_target))
        if not os.path.exists(exe_abs):
            self.skipTest("gpu async engine firmware not found at {}; "
                          "run sysmode/firmware/build.sh".format(exe_abs))

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "quetz_sysmode_tests", "gpu_async_engine")
        os.makedirs(outdir, exist_ok=True)

        sdlfile     = os.path.join(test_path, "sysmode", "basic_quetz_gpu.py")
        sst_outfile = os.path.join(outdir, "gpu_async_engine.out")
        sst_errfile = os.path.join(outdir, "gpu_async_engine.err")
        mpifiles    = os.path.join(outdir, "gpu_async_engine.testfile")

        memmaps = [
            ("kernel_dram", 0x80000000, 0x800FFFFF, "filtered"),
            ("sub_ram",     0x00000000, 0x7FFFFFFF, "filtered"),
        ]
        make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                         "-machine virt -nographic -bios none",
                         "-kernel", 0, 0xFFFFFFFF, memmaps)
        os.environ["QUETZ_MMIO_START"] = "0x80100000"
        os.environ["QUETZ_MMIO_END"]   = "0x801003FF"
        os.environ["QUETZ_ASYNC_OFFLOAD"] = "1"
        os.environ["QUETZ_GPU_LATENCY"]   = "40000"
        enable_mmio_payload_delivery()

        self.run_sst(sdlfile, sst_outfile, sst_errfile,
                     mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=180)

        # Leave async mode off for any test that runs after this one in-process.
        os.environ.pop("QUETZ_ASYNC_OFFLOAD", None)
        os.environ.pop("QUETZ_GPU_LATENCY", None)

        with open(sst_outfile, "r") as f:
            raw_output = f.read()

        self.assertIn("ASYNC ENGINE OK", raw_output,
            "async engine did not complete two posted offloads correctly")

        kernels_launched = stat_sum(raw_output, "gpu.kernels_launched")
        busy_cycles      = stat_sum(raw_output, "gpu.busy_cycles")
        flushes          = stat_sum(raw_output, "cpu.mmio_doorbell_flushes")

        self.assertEqual(kernels_launched, 2,
            "engine forwards two posted doorbells")
        self.assertIsNotNone(busy_cycles, "gpu.busy_cycles not found")
        self.assertGreaterEqual(busy_cycles, 2 * 40000 - 4,
            "both posted kernels run to completion (blocking doorbell)")
        # The coherence bridge must still flush the scratch range on the posted
        # path before the doorbell reaches the device.
        self.assertIsNotNone(flushes, "cpu.mmio_doorbell_flushes not found")
        self.assertGreater(flushes, 0,
            "posted offload must flush the scratch range before forwarding")

    # -------------------------------------------------------------------------
    def test_quetz_sysmode_gpu_async_explicit(self):
        """P5 extensibility: configure the accelerator via the explicit
        'accelerator' subcomponent slot (not the legacy cpu-param auto-shim),
        with flush_mode=none.

        Same async-engine firmware and result, but the port is instantiated by
        config (setSubComponent) and its coherence policy is changed to 'none' —
        so no FlushAddr requests are issued, yet the offload still completes
        correctly. Demonstrates that a different accelerator policy is a config
        change with no engine edits."""
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec = sst_paths()

        qemu_target = "qemu-system-riscv64"
        exe_rel     = "sysmode/firmware/riscv_virt_gpu_async_engine"
        import shutil
        qemu_bin = os.path.join(sst_bindir, qemu_target)
        if not os.path.exists(qemu_bin):
            found = shutil.which(qemu_target)
            if found:
                qemu_bin = found
        exe_abs = os.path.normpath(os.path.join(test_path, exe_rel))

        if not os.path.exists(qemu_bin):
            self.skipTest("{} not found; skipping".format(qemu_target))
        if not os.path.exists(exe_abs):
            self.skipTest("gpu async engine firmware not found at {}; "
                          "run sysmode/firmware/build.sh".format(exe_abs))

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "quetz_sysmode_tests", "gpu_async_explicit")
        os.makedirs(outdir, exist_ok=True)

        sdlfile     = os.path.join(test_path, "sysmode", "basic_quetz_gpu.py")
        sst_outfile = os.path.join(outdir, "gpu_async_explicit.out")
        sst_errfile = os.path.join(outdir, "gpu_async_explicit.err")
        mpifiles    = os.path.join(outdir, "gpu_async_explicit.testfile")

        memmaps = [
            ("kernel_dram", 0x80000000, 0x800FFFFF, "filtered"),
            ("sub_ram",     0x00000000, 0x7FFFFFFF, "filtered"),
        ]
        make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                         "-machine virt -nographic -bios none",
                         "-kernel", 0, 0xFFFFFFFF, memmaps)
        os.environ["QUETZ_MMIO_START"]    = "0x80100000"
        os.environ["QUETZ_MMIO_END"]      = "0x801003FF"
        os.environ["QUETZ_ASYNC_OFFLOAD"] = "1"
        os.environ["QUETZ_GPU_LATENCY"]   = "40000"
        os.environ["QUETZ_ACCEL_EXPLICIT"]   = "1"
        os.environ["QUETZ_ACCEL_FLUSH_MODE"] = "none"
        enable_mmio_payload_delivery()

        self.run_sst(sdlfile, sst_outfile, sst_errfile,
                     mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=180)

        for k in ("QUETZ_ASYNC_OFFLOAD", "QUETZ_GPU_LATENCY",
                  "QUETZ_ACCEL_EXPLICIT", "QUETZ_ACCEL_FLUSH_MODE"):
            os.environ.pop(k, None)

        with open(sst_outfile, "r") as f:
            raw_output = f.read()

        self.assertIn("ASYNC ENGINE OK", raw_output,
            "explicit-slot async engine did not complete correctly")
        kernels_launched = stat_sum(raw_output, "gpu.kernels_launched")
        flushes          = stat_sum(raw_output, "cpu.mmio_doorbell_flushes")
        self.assertEqual(kernels_launched, 2,
            "engine forwards two posted doorbells")
        # flush_mode=none → the coherence bridge is disabled by config.
        self.assertEqual(flushes, 0,
            "flush_mode=none must issue no FlushAddr requests")

    # -------------------------------------------------------------------------
    def test_quetz_sysmode_gpu_async_queue(self):
        """P4 completion queue: three posted offloads in flight, FIFO completion.

        The guest posts three ops without waiting (async_completion_depth=4); the
        engine forwards them to the synthetic GPU one at a time and the COMPLETED
        counter advances 1->2->3 in submission order. Validates the queue + the
        per-op deferred flush, balar-free."""
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec = sst_paths()

        qemu_target = "qemu-system-riscv64"
        exe_rel     = "sysmode/firmware/riscv_virt_gpu_async_queue"
        import shutil
        qemu_bin = os.path.join(sst_bindir, qemu_target)
        if not os.path.exists(qemu_bin):
            found = shutil.which(qemu_target)
            if found:
                qemu_bin = found
        exe_abs = os.path.normpath(os.path.join(test_path, exe_rel))

        if not os.path.exists(qemu_bin):
            self.skipTest("{} not found; skipping".format(qemu_target))
        if not os.path.exists(exe_abs):
            self.skipTest("gpu async queue firmware not found at {}; "
                          "run sysmode/firmware/build.sh".format(exe_abs))

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "quetz_sysmode_tests", "gpu_async_queue")
        os.makedirs(outdir, exist_ok=True)

        sdlfile     = os.path.join(test_path, "sysmode", "basic_quetz_gpu.py")
        sst_outfile = os.path.join(outdir, "gpu_async_queue.out")
        sst_errfile = os.path.join(outdir, "gpu_async_queue.err")
        mpifiles    = os.path.join(outdir, "gpu_async_queue.testfile")

        memmaps = [
            ("kernel_dram", 0x80000000, 0x800FFFFF, "filtered"),
            ("sub_ram",     0x00000000, 0x7FFFFFFF, "filtered"),
        ]
        make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                         "-machine virt -nographic -bios none",
                         "-kernel", 0, 0xFFFFFFFF, memmaps)
        os.environ["QUETZ_MMIO_START"] = "0x80100000"
        os.environ["QUETZ_MMIO_END"]   = "0x801003FF"
        os.environ["QUETZ_ASYNC_OFFLOAD"] = "1"
        os.environ["QUETZ_ASYNC_DEPTH"]   = "4"
        os.environ["QUETZ_GPU_LATENCY"]   = "20000"
        enable_mmio_payload_delivery()

        self.run_sst(sdlfile, sst_outfile, sst_errfile,
                     mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=180)

        for k in ("QUETZ_ASYNC_OFFLOAD", "QUETZ_ASYNC_DEPTH", "QUETZ_GPU_LATENCY"):
            os.environ.pop(k, None)

        with open(sst_outfile, "r") as f:
            raw_output = f.read()

        self.assertIn("ASYNC QUEUE OK", raw_output,
            "three posted offloads did not complete in FIFO order")
        kernels_launched = stat_sum(raw_output, "gpu.kernels_launched")
        self.assertEqual(kernels_launched, 3,
            "engine forwards three posted doorbells, one at a time")
        # COMPLETED must advance strictly 1 -> 2 -> 3 (FIFO).
        self.assertIn("completed_id=1", raw_output)
        self.assertIn("completed_id=2", raw_output)
        self.assertIn("completed_id=3", raw_output)

    # -------------------------------------------------------------------------
    def _quetz_balar_sysmode_template(self, testname, timeout_sec,
                                      exe_rel="sysmode/firmware/riscv_virt_balar_kernel",
                                      cuda_binary="vectorAdd", async_offload=False):
        if not os.environ.get("GPGPUSIM_ROOT"):
            self.skipTest("GPGPUSIM_ROOT not set; skipping sysmode_balar test")

        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec = sst_paths()

        qemu_target = "qemu-system-riscv64"
        import shutil
        qemu_bin = os.path.join(sst_bindir, qemu_target)
        if not os.path.exists(qemu_bin):
            found = shutil.which(qemu_target)
            if found:
                qemu_bin = found

        exe_abs = os.path.normpath(os.path.join(test_path, exe_rel))
        balar_tests = os.path.normpath(os.path.join(test_path, "../../balar/tests"))
        cfg_file = os.path.join(balar_tests, "gpu-v100-mem.cfg")
        cuda_exe = os.path.join(balar_tests, "balar_trace", cuda_binary)
        # GPGPU-Sim reads a file literally named "gpgpusim.config" from the
        # process CWD at init; the balar testsuite symlinks it into its run dir.
        # We run with set_cwd=outdir, so it must be present there too.
        gpgpusim_cfg = os.path.join(balar_tests, "gpgpusim.config")

        if not os.path.exists(qemu_bin):
            self.skipTest("{} not found; skipping".format(qemu_target))
        if not os.path.exists(exe_abs):
            self.skipTest("balar firmware not found at {}; "
                          "run sysmode/firmware/build.sh".format(exe_abs))
        if not os.path.exists(cfg_file):
            self.skipTest("Balar GPGPU-Sim config not found at {}".format(cfg_file))
        if not os.path.exists(gpgpusim_cfg):
            self.skipTest("gpgpusim.config not found at {}".format(gpgpusim_cfg))
        if not os.path.exists(cuda_exe):
            self.skipTest("Balar {} binary not found at {}; "
                          "build balar/tests/balar_trace".format(cuda_binary, cuda_exe))

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "quetz_sysmode_tests", testname)
        os.makedirs(outdir, exist_ok=True)
        import shutil as _shutil
        _shutil.copy(gpgpusim_cfg, os.path.join(outdir, "gpgpusim.config"))

        sdlfile = os.path.join(test_path, "sysmode", "basic_quetz_balar.py")
        sst_outfile = os.path.join(outdir, testname + ".out")
        sst_errfile = os.path.join(outdir, testname + ".err")
        mpifiles = os.path.join(outdir, testname + ".testfile")

        # The SDL splits the coherent fabric into low boot/device timing
        # ([0, balar_mmio)) and high DRAM ([0x80000000, ram_end]) so the
        # balar MMIO window remains a disjoint network peer.
        make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                         "-machine virt -nographic -bios none",
                         "-kernel", 0, 0xFFFFFFFF, [])
        os.environ["QUETZ_MMIO_START"] = "0x70000000"
        os.environ["QUETZ_MMIO_END"] = "0x700005FF"
        os.environ["BALAR_CONFIG"] = cfg_file
        os.environ["BALAR_CUDA_EXE_PATH"] = cuda_exe
        os.environ["BALAR_VERBOSE"] = "1"
        os.environ["BALAR_DMA_VERBOSE"] = "0"
        os.environ.pop("QUETZ_PLATFORM", None)
        if async_offload:
            os.environ["QUETZ_ASYNC_OFFLOAD"] = "1"
        else:
            os.environ.pop("QUETZ_ASYNC_OFFLOAD", None)
        enable_mmio_payload_delivery()

        self.run_sst(sdlfile, sst_outfile, sst_errfile,
                     mpi_out_files=mpifiles, set_cwd=outdir,
                     timeout_sec=timeout_sec)

        os.environ.pop("QUETZ_ASYNC_OFFLOAD", None)

        raw = ""
        if os.path.exists(sst_outfile):
            with open(sst_outfile, "r") as f:
                raw += f.read()
        err_text = ""
        if os.path.exists(sst_errfile):
            with open(sst_errfile, "r") as f:
                err_text = f.read()
            raw += "\n" + err_text
        self.assertNotIn("FATAL", raw)

        stats = parse_stats(sst_outfile)
        flushes = stats.get("cpu.mmio_doorbell_flushes.0", 0)
        # Surface what the sim actually did so a flushes==0 failure is debuggable
        # without re-running locally. These are the breadcrumbs the handoff asks
        # for first: did the guest even reach the doorbell?
        mmio_writes  = stats.get("cpu.mmio_write_requests.0", 0)
        mmio_reads   = stats.get("cpu.mmio_read_requests.0", 0)
        plugin_attached = "Plugin attached!" in raw
        diag = (
            "mmio_doorbell_flushes={f} mmio_writes={w} mmio_reads={r} "
            "plugin_attached={a} stat_lines={n}"
        ).format(f=flushes, w=mmio_writes, r=mmio_reads,
                 a=plugin_attached, n=len(stats))
        # If the sim produced no stats at all, SST almost certainly aborted
        # during SDL config (a Python exception in basic_quetz_balar.py /
        # balarBlock.build) — which does NOT contain "FATAL". Embed the tail of
        # stdout/stderr right in the failure so the CI test log shows the real
        # error without needing downloadable artifacts.
        if flushes < 1 and len(stats) == 0:
            tail = "\n".join((err_text or raw).splitlines()[-40:])
            diag += "\n--- SST output tail ---\n" + tail
        self.assertGreaterEqual(flushes, 1,
            "balar doorbell path did not issue any FlushAddr(inv) requests; "
            + diag)
        return raw, stats, flushes

    # -------------------------------------------------------------------------
    def test_quetz_balar_smoke(self):
        raw, stats, _flushes = self._quetz_balar_sysmode_template(
            "quetz_balar_smoke", 60 * 20)
        self.assertGreaterEqual(stats.get("cpu.mmio_write_requests.0", 0), 1)
        self.assertIn("Handling CUDA API Call", raw,
            "Balar did not log any CUDA API packet handling")

    # -------------------------------------------------------------------------
    def test_quetz_balar_vectoradd(self):
        raw, _stats, flushes = self._quetz_balar_sysmode_template(
            "quetz_balar_vectoradd", 60 * 40)
        self.assertIn("Kernel_done", raw,
            "GPGPU-Sim/vectorAdd completion marker not observed")
        self.assertIn("correct_memD2H_ratio=", raw,
            "firmware did not print D2H correctness ratio")

        marker = "correct_memD2H_ratio="
        start = raw.rfind(marker)
        ratio_text = raw[start + len(marker):].splitlines()[0]
        correct_s, total_s = ratio_text.split("/", 1)
        correct = float(correct_s)
        total = float(total_s)
        self.assertGreater(total, 0)
        self.assertGreaterEqual(correct / total, 0.95)
        self.assertGreaterEqual(raw.count("Handling CUDA API Call"), 18,
            "expected the full firmware CUDA packet stream to reach Balar")
        self.assertGreaterEqual(flushes, 18,
            "expected at least one doorbell flush per CUDA API packet")

    # -------------------------------------------------------------------------
    def test_quetz_balar_vectoradd_async(self):
        """P4 async offload through balar: vectorAdd with a posted
        cudaThreadSynchronize.

        Identical vectorAdd, but the post-launch sync is posted via the Quetz
        async aperture: balar holds its deferred write response until the kernel
        completes while the guest runs CPU work and polls the completion counter.
        The result must still be bit-exact with the synchronous path (the async
        change is *when* the guest waits, not *what* balar computes)."""
        raw, stats, flushes = self._quetz_balar_sysmode_template(
            "quetz_balar_vectoradd_async", 60 * 40,
            exe_rel="sysmode/firmware/riscv_virt_balar_async",
            async_offload=True)
        self.assertIn("Balar vectorAdd ASYNC done", raw,
            "async firmware did not reach completion")
        self.assertIn("async offload completed", raw,
            "Quetz async engine did not retire the posted offload")
        self.assertEqual(stats.get("cpu.async_submits.0", 0), 1,
            "expected exactly one posted thread-sync")
        self.assertEqual(stats.get("cpu.async_completions.0", 0), 1,
            "posted offload must complete exactly once")
        # The headline win: the vCPU made progress while the offload was in
        # flight (overlap), rather than blocking for the whole GPU op.
        self.assertGreater(stats.get("cpu.async_overlap_cycles.0", 0), 0,
            "async offload must overlap with vCPU execution")

        marker = "correct_memD2H_ratio="
        start = raw.rfind(marker)
        ratio_text = raw[start + len(marker):].splitlines()[0]
        correct_s, total_s = ratio_text.split("/", 1)
        correct = float(correct_s)
        total = float(total_s.split()[0])
        self.assertEqual(total, 256)
        self.assertEqual(correct, total,
            "async vectorAdd must be bit-exact with the synchronous result")
        self.assertGreaterEqual(flushes, 18,
            "expected at least one doorbell flush per CUDA API packet")

    # -------------------------------------------------------------------------
    def test_quetz_balar_fft(self):
        """GPU-driven staged radix-2 FFT (impulse, N=256) through Quetz->Balar.

        The impulse FFT is bit-exact (every bin = 1+0j), so the firmware's
        on-guest word check must be perfect (correct == total). The 1+log2(N)
        kernel launches make this a much heavier host<->GPU command stream than
        vectorAdd, which is the point for the architectural demo.
        """
        raw, _stats, flushes = self._quetz_balar_sysmode_template(
            "quetz_balar_fft", 60 * 40,
            exe_rel="sysmode/firmware/riscv_virt_balar_fft",
            cuda_binary="fft")
        self.assertIn("FFT Kernel_done", raw,
            "GPGPU-Sim/FFT completion marker not observed")
        self.assertIn("correct_words=", raw,
            "firmware did not print the FFT word-correctness count")

        marker = "correct_words="
        start = raw.rfind(marker)
        ratio_text = raw[start + len(marker):].splitlines()[0]
        correct_s, total_s = ratio_text.split("/", 1)
        correct = float(correct_s)
        total = float(total_s)
        self.assertEqual(total, 512,
            "N=256 complex output should be 512 words (re,im)")
        self.assertEqual(correct, total,
            "impulse FFT must be bit-exact: every output word 1.0f/0.0f")
        # bit-reversal + log2(256) stages = 9 launches; each launch is a
        # config + set_args + launch + sync packet group, so the command stream
        # is well above vectorAdd's.
        self.assertGreaterEqual(raw.count("Handling CUDA API Call"), 40,
            "expected the full staged-FFT CUDA packet stream to reach Balar")
        self.assertGreaterEqual(flushes, 40,
            "expected at least one doorbell flush per CUDA API packet")

    # -------------------------------------------------------------------------
    def test_quetz_usermode_gpu_kernel(self):
        """User-mode QuetzGpuDevice: mmio_link doorbell + timed BUSY/IDLE."""
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec = sst_paths()

        qemu_target = "qemu-riscv64"
        exe_rel     = "binaries/gpu_kernel_user"
        import shutil
        qemu_bin = os.path.join(sst_bindir, qemu_target)
        if not os.path.exists(qemu_bin):
            found = shutil.which(qemu_target)
            if found:
                qemu_bin = found
        exe_abs = os.path.normpath(os.path.join(test_path, exe_rel))

        if not os.path.exists(qemu_bin):
            self.skipTest("{} not found; skipping".format(qemu_target))
        src_abs = os.path.normpath(os.path.join(
            test_path, "usermode", "sources", "gpu_kernel_user.c"))
        if not os.path.exists(exe_abs):
            if os.path.exists(src_abs):
                self.fail("gpu_kernel_user binary missing at {}; "
                          "run usermode/sources/build.sh".format(exe_abs))
            self.skipTest("gpu_kernel_user not found at {}; "
                          "run usermode/sources/build.sh".format(exe_abs))

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "quetz_tests", "usermode_gpu_kernel")
        os.makedirs(outdir, exist_ok=True)

        sdlfile     = os.path.join(test_path, "usermode", "basic_quetz_gpu.py")
        sst_outfile = os.path.join(outdir, "usermode_gpu_kernel.out")
        sst_errfile = os.path.join(outdir, "usermode_gpu_kernel.err")
        mpifiles    = os.path.join(outdir, "usermode_gpu_kernel.testfile")

        make_usermode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                          with_l1=False, isa="", detailed=False)
        os.environ["QUETZ_MMIO_START"] = "0x80100000"
        os.environ["QUETZ_MMIO_END"]   = "0x801003FF"
        # NOTE: usermode SIGSEGV hook deferred — plugin trace path only.
        apply_usermode_region_handlers([
            ("kernel_dram", 0x80000000, 0x800FFFFF, "filtered"),
            ("sub_ram",     0x00000000, 0x7FFFFFFF, "filtered"),
            # QEMU user-mode stack/heap live above 4 GiB; filter so MMIO stats stay clean.
            ("user_high",   0x80100400, (1 << 48) - 1, "filtered"),
        ])

        self.run_sst(sdlfile, sst_outfile, sst_errfile,
                     mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=180)

        stats = parse_stats(sst_outfile)
        with open(sst_outfile, "r") as f:
            raw_output = f.read()

        mmio_writes = stats.get("cpu.mmio_write_requests.0", 0)
        mmio_reads  = stats.get("cpu.mmio_read_requests.0", 0)
        cache_writes = stats.get("cpu.write_requests.0", 0)

        kernels_launched = stat_sum(raw_output, "gpu.kernels_launched")
        busy_cycles = stat_sum(raw_output, "gpu.busy_cycles")

        self.assertGreaterEqual(mmio_writes, 6,
            "expected doorbell + latency_override MMIO writes")
        self.assertGreaterEqual(mmio_reads, 3,
            "expected KERNEL_ID reads (user-mode guest does not spin on STATUS)")
        self.assertEqual(cache_writes, 0,
            "GPU doorbell must not escape to cache_link")
        self.assertIsNotNone(kernels_launched,
            "gpu.kernels_launched not found in output")
        self.assertEqual(kernels_launched, 3,
            "guest launches exactly three kernels (no STATUS spin in user-mode)")
        self.assertIsNotNone(busy_cycles,
            "gpu.busy_cycles not found in output")
        self.assertGreater(busy_cycles, 0,
            "GPU should accumulate busy_cycles during kernel runs")

    # -------------------------------------------------------------------------
    def test_quetz_usermode_gpu_trace_capture(self):
        """User-mode GpuTraceRegionHandler: doorbell + status poll capture."""
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec = sst_paths()

        qemu_target = "qemu-riscv64"
        exe_rel     = "binaries/gpu_trace_user"
        import shutil
        qemu_bin = os.path.join(sst_bindir, qemu_target)
        if not os.path.exists(qemu_bin):
            found = shutil.which(qemu_target)
            if found:
                qemu_bin = found
        exe_abs = os.path.normpath(os.path.join(test_path, exe_rel))

        if not os.path.exists(qemu_bin):
            self.skipTest("{} not found; skipping".format(qemu_target))
        src_abs = os.path.normpath(os.path.join(
            test_path, "usermode", "sources", "gpu_trace_user.c"))
        if not os.path.exists(exe_abs):
            if os.path.exists(src_abs):
                self.fail("gpu_trace_user binary missing at {}; "
                          "run usermode/sources/build.sh".format(exe_abs))
            self.skipTest("gpu_trace_user not found at {}; "
                          "run usermode/sources/build.sh".format(exe_abs))

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "quetz_tests", "usermode_gpu_trace")
        os.makedirs(outdir, exist_ok=True)

        sdlfile     = os.path.join(test_path, "usermode", "basic_quetz_gpu_trace.py")
        sst_outfile = os.path.join(outdir, "usermode_gpu_trace.out")
        sst_errfile = os.path.join(outdir, "usermode_gpu_trace.err")
        mpifiles    = os.path.join(outdir, "usermode_gpu_trace.testfile")

        make_usermode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                          with_l1=False, isa="", detailed=False)
        os.environ["QUETZ_MMIO_START"] = "0x80100000"
        os.environ["QUETZ_MMIO_END"]   = "0x801003FF"
        apply_usermode_region_handlers([
            ("sub_ram",   0x00000000, 0x7FFFFFFF, "filtered"),
            ("user_high", 0x80100400, (1 << 48) - 1, "filtered"),
        ])

        self.run_sst(sdlfile, sst_outfile, sst_errfile,
                     mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=120)

        stats = parse_stats(sst_outfile)
        with open(sst_outfile, "r") as f:
            raw = f.read()

        self.assertEqual(stats.get("cpu.gpu_doorbell_writes.0", 0), 1)
        self.assertEqual(stats.get("cpu.gpu_status_polls.0", 0), 8)
        self.assertIn("GPU_TRACE[0]:", raw)
        idx = raw.find("GPU_TRACE[0]:")
        self.assertNotEqual(idx, -1)
        line = raw[idx:raw.find("\n", idx)]
        self.assertIn("doorbells=1", line)
        self.assertIn("polls=8", line)
        self.assertIn("deadbeef", line.lower())

    # -------------------------------------------------------------------------
    def test_quetz_sysmode_uart_capture(self):
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec = sst_paths()

        qemu_target = "qemu-system-riscv64"
        exe_rel     = "sysmode/firmware/riscv_virt_uart_echo"
        import shutil
        qemu_bin = os.path.join(sst_bindir, qemu_target)
        if not os.path.exists(qemu_bin):
            found = shutil.which(qemu_target)
            if found:
                qemu_bin = found
        exe_abs = os.path.normpath(os.path.join(test_path, exe_rel))

        if not os.path.exists(qemu_bin) or not os.path.exists(exe_abs):
            self.skipTest("uart echo firmware missing; skipping")

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "quetz_sysmode_tests", "uart_capture")
        os.makedirs(outdir, exist_ok=True)

        stdin_path = os.path.join(outdir, "uart_stdin.bin")
        echo_input = b"ABCDE"
        with open(stdin_path, "wb") as f:
            f.write(echo_input)

        memmaps = [
            ("uart0", 0x10000000, 0x10000FFF, "uart"),
            ("sub_ram", 0x00000000, 0x7FFFFFFF, "filtered"),
        ]
        make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                         "-machine virt -nographic -bios none",
                         "-kernel", 0, 0xFFFFFFFF, memmaps,
                         stdin_file=stdin_path)

        sdlfile     = os.path.join(test_path, "sysmode", "basic_quetz_sysmode.py")
        sst_outfile = os.path.join(outdir, "uart_capture.out")
        sst_errfile = os.path.join(outdir, "uart_capture.err")
        mpifiles    = os.path.join(outdir, "uart_capture.testfile")

        self.run_sst(sdlfile, sst_outfile, sst_errfile,
                     mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=120)

        with open(sst_outfile, "r") as f:
            raw = f.read()
        self.assertIn("UART[0]:", raw)
        idx = raw.find("UART[0]:")
        self.assertNotEqual(idx, -1)
        line = raw[idx:raw.find("\n", idx)]
        for ch in echo_input:
            self.assertIn(chr(ch), line,
                "UART capture missing byte {!r}".format(ch))

    # -------------------------------------------------------------------------
    def test_quetz_sysmode_gpu_trace_capture(self):
        test_path = self.get_testsuite_dir()
        sst_prefix, sst_bindir, sst_libexec = sst_paths()

        qemu_target = "qemu-system-riscv64"
        exe_rel     = "sysmode/firmware/riscv_virt_gpu_trace"
        import shutil
        qemu_bin = os.path.join(sst_bindir, qemu_target)
        if not os.path.exists(qemu_bin):
            found = shutil.which(qemu_target)
            if found:
                qemu_bin = found
        exe_abs = os.path.normpath(os.path.join(test_path, exe_rel))

        if not os.path.exists(qemu_bin):
            self.skipTest("{} not found; skipping".format(qemu_target))
        if not os.path.exists(exe_abs):
            self.skipTest("gpu trace firmware not found at {}; "
                          "run sysmode/firmware/build.sh".format(exe_abs))

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "quetz_sysmode_tests", "gpu_trace_capture")
        os.makedirs(outdir, exist_ok=True)

        memmaps = [
            ("gpu_mmio", 0x80100000, 0x801003FF, "gpu_trace",
                {"doorbell_offset": 0, "status_offset": 8}),
            ("uart0",    0x10000000, 0x10000FFF, "uart"),
            ("sub_ram",  0x00000000, 0x7FFFFFFF, "filtered"),
        ]
        make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                         "-machine virt -nographic -bios none",
                         "-kernel", 0, 0xFFFFFFFF, memmaps)

        sdlfile     = os.path.join(test_path, "sysmode", "basic_quetz_sysmode.py")
        sst_outfile = os.path.join(outdir, "gpu_trace_capture.out")
        sst_errfile = os.path.join(outdir, "gpu_trace_capture.err")
        mpifiles    = os.path.join(outdir, "gpu_trace_capture.testfile")

        self.run_sst(sdlfile, sst_outfile, sst_errfile,
                     mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=120)

        with open(sst_outfile, "r") as f:
            raw = f.read()
        self.assertIn("GPU_TRACE[0]:", raw)
        idx = raw.find("GPU_TRACE[0]:")
        self.assertNotEqual(idx, -1)
        line = raw[idx:raw.find("\n", idx)]
        self.assertIn("doorbells=1", line)
        self.assertIn("polls=8", line)
        self.assertIn("deadbeef", line.lower())

    # -------------------------------------------------------------------------
    def test_quetz_coldfire_monitor(self):
        """ColdFire (m68k) dBUG-style monitor + GPU offload through Quetz→balar.

        A 32-bit big-endian NXP MCF5208 boots, prints a banner over UART TX,
        reads commands over UART RX, and dispatches a vectorAdd to
        balar/GPGPU-Sim. Checks the full UART transcript and that the GPU
        result is bit-exact (256/256).

        Requires qemu-system-m68k (QEMU built with m68k-softmmu) and the
        coldfire_monitor firmware (M68K_CC=m68k-linux-gnu-gcc ./build.sh).
        """
        if not os.environ.get("GPGPUSIM_ROOT"):
            self.skipTest("GPGPUSIM_ROOT not set; skipping coldfire_monitor test")

        test_path = self.get_testsuite_dir()
        sst_prefix  = sstsimulator_conf_get_value("SSTCore", "prefix",     str, "")
        sst_bindir  = sstsimulator_conf_get_value("SSTCore", "bindir",     str, "")
        sst_libexec = sstsimulator_conf_get_value("SSTCore", "libexecdir", str, "")

        qemu_target = "qemu-system-m68k"
        import shutil
        qemu_bin = os.path.join(sst_bindir, qemu_target)
        if not os.path.exists(qemu_bin):
            found = shutil.which(qemu_target)
            if found:
                qemu_bin = found

        exe_abs = os.path.normpath(os.path.join(
            test_path, "sysmode/firmware/coldfire_monitor"))
        balar_tests = os.path.normpath(os.path.join(test_path, "../../balar/tests"))
        cfg_file = os.path.join(balar_tests, "gpu-v100-mem.cfg")
        cuda_exe = os.path.join(balar_tests, "balar_trace", "vectorAdd")
        gpgpusim_cfg = os.path.join(balar_tests, "gpgpusim.config")

        if not os.path.exists(qemu_bin):
            self.skipTest(
                "{} not found; rebuild QEMU with m68k-softmmu".format(qemu_target))
        if not os.path.exists(exe_abs):
            self.skipTest(
                "coldfire_monitor not found at {}; "
                "run M68K_CC=m68k-linux-gnu-gcc ./build.sh".format(exe_abs))
        if not os.path.exists(cfg_file):
            self.skipTest("Balar config not found at {}".format(cfg_file))
        if not os.path.exists(gpgpusim_cfg):
            self.skipTest("gpgpusim.config not found at {}".format(gpgpusim_cfg))
        if not os.path.exists(cuda_exe):
            self.skipTest(
                "vectorAdd not found at {}; build balar/tests/balar_trace".format(
                    cuda_exe))

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "quetz_sysmode_tests", "coldfire_monitor")
        os.makedirs(outdir, exist_ok=True)
        import shutil as _shutil
        _shutil.copy(gpgpusim_cfg, os.path.join(outdir, "gpgpusim.config"))

        stdin_path = os.path.join(outdir, "coldfire_cmds.txt")
        with open(stdin_path, "w") as f:
            f.write("help\ngpu\nquit\n")

        make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                         "-machine mcf5208evb -display none -serial stdio -m 128M",
                         "-kernel", 0x40000000, 0x47FFFFFF, [],
                         stdin_file=stdin_path)
        os.environ["QUETZ_MMIO_START"] = "0x70000000"
        os.environ["QUETZ_MMIO_END"]   = "0x700005FF"
        os.environ["BALAR_CONFIG"] = cfg_file
        os.environ["BALAR_CUDA_EXE_PATH"] = cuda_exe
        os.environ["BALAR_VERBOSE"] = "1"
        os.environ["BALAR_DMA_VERBOSE"] = "0"
        os.environ.pop("QUETZ_PLATFORM", None)
        enable_mmio_payload_delivery()

        sdlfile     = os.path.join(test_path, "sysmode", "basic_quetz_balar_coldfire.py")
        sst_outfile = os.path.join(outdir, "coldfire_monitor.out")
        sst_errfile = os.path.join(outdir, "coldfire_monitor.err")
        mpifiles    = os.path.join(outdir, "coldfire_monitor.testfile")

        self.run_sst(sdlfile, sst_outfile, sst_errfile,
                     mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=60 * 40)

        raw = ""
        if os.path.exists(sst_outfile):
            with open(sst_outfile, "r") as f:
                raw += f.read()
        if os.path.exists(sst_errfile):
            with open(sst_errfile, "r") as f:
                raw += "\n" + f.read()
        self.assertNotIn("FATAL", raw)

        self.assertIn("ColdFire dBUG-style monitor", raw,
            "ColdFire UART banner not captured; firmware may not have booted")
        self.assertIn("dbug>", raw,
            "ColdFire monitor prompt not captured; UART RX injection may have failed")
        self.assertIn("gpu vectorAdd correct=256/256", raw,
            "ColdFire GPU command did not report bit-exact vectorAdd result")
        self.assertIn("TESTFINISH[0]", raw,
            "TestFinisherRegionHandler sentinel not triggered; sim may have hung")

        stats = parse_stats(sst_outfile)
        flushes = stats.get("cpu.mmio_doorbell_flushes.0", 0)
        self.assertGreaterEqual(flushes, 1,
            "balar doorbell path issued no FlushAddr requests from ColdFire host")

    # -------------------------------------------------------------------------
    def test_quetz_coldfire_gpu_async(self):
        """ColdFire (m68k) P4 async offload: vectorAdd with a posted
        cudaThreadSynchronize through Quetz->balar.

        The 32-bit big-endian core posts the post-launch sync via the Quetz async
        aperture, runs CPU work while balar runs the kernel, then polls the
        completion counter. Result must be bit-exact (256/256) with the
        synchronous ColdFire path, and the overlap must be non-zero. Exercises
        the async engine with a 4-byte (vs RISC-V 8-byte) doorbell width."""
        if not os.environ.get("GPGPUSIM_ROOT"):
            self.skipTest("GPGPUSIM_ROOT not set; skipping coldfire_gpu_async test")

        test_path = self.get_testsuite_dir()
        sst_prefix  = sstsimulator_conf_get_value("SSTCore", "prefix",     str, "")
        sst_bindir  = sstsimulator_conf_get_value("SSTCore", "bindir",     str, "")
        sst_libexec = sstsimulator_conf_get_value("SSTCore", "libexecdir", str, "")

        qemu_target = "qemu-system-m68k"
        import shutil
        qemu_bin = os.path.join(sst_bindir, qemu_target)
        if not os.path.exists(qemu_bin):
            found = shutil.which(qemu_target)
            if found:
                qemu_bin = found

        exe_abs = os.path.normpath(os.path.join(
            test_path, "sysmode/firmware/coldfire_gpu_async"))
        balar_tests = os.path.normpath(os.path.join(test_path, "../../balar/tests"))
        cfg_file = os.path.join(balar_tests, "gpu-v100-mem.cfg")
        cuda_exe = os.path.join(balar_tests, "balar_trace", "vectorAdd")
        gpgpusim_cfg = os.path.join(balar_tests, "gpgpusim.config")

        if not os.path.exists(qemu_bin):
            self.skipTest(
                "{} not found; rebuild QEMU with m68k-softmmu".format(qemu_target))
        if not os.path.exists(exe_abs):
            self.skipTest(
                "coldfire_gpu_async not found at {}; "
                "run M68K_CC=m68k-linux-gnu-gcc ./build.sh".format(exe_abs))
        if not os.path.exists(cfg_file):
            self.skipTest("Balar config not found at {}".format(cfg_file))
        if not os.path.exists(gpgpusim_cfg):
            self.skipTest("gpgpusim.config not found at {}".format(gpgpusim_cfg))
        if not os.path.exists(cuda_exe):
            self.skipTest(
                "vectorAdd not found at {}; build balar/tests/balar_trace".format(
                    cuda_exe))

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "quetz_sysmode_tests", "coldfire_gpu_async")
        os.makedirs(outdir, exist_ok=True)
        import shutil as _shutil
        _shutil.copy(gpgpusim_cfg, os.path.join(outdir, "gpgpusim.config"))

        make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                         "-machine mcf5208evb -display none -serial stdio -m 128M",
                         "-kernel", 0x40000000, 0x47FFFFFF, [])
        os.environ["QUETZ_MMIO_START"] = "0x70000000"
        os.environ["QUETZ_MMIO_END"]   = "0x700005FF"
        os.environ["BALAR_CONFIG"] = cfg_file
        os.environ["BALAR_CUDA_EXE_PATH"] = cuda_exe
        os.environ["BALAR_VERBOSE"] = "1"
        os.environ["BALAR_DMA_VERBOSE"] = "0"
        os.environ["QUETZ_ASYNC_OFFLOAD"] = "1"
        os.environ.pop("QUETZ_PLATFORM", None)
        enable_mmio_payload_delivery()

        sdlfile     = os.path.join(test_path, "sysmode", "basic_quetz_balar_coldfire.py")
        sst_outfile = os.path.join(outdir, "coldfire_gpu_async.out")
        sst_errfile = os.path.join(outdir, "coldfire_gpu_async.err")
        mpifiles    = os.path.join(outdir, "coldfire_gpu_async.testfile")

        self.run_sst(sdlfile, sst_outfile, sst_errfile,
                     mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=60 * 40)

        os.environ.pop("QUETZ_ASYNC_OFFLOAD", None)

        raw = ""
        if os.path.exists(sst_outfile):
            with open(sst_outfile, "r") as f:
                raw += f.read()
        if os.path.exists(sst_errfile):
            with open(sst_errfile, "r") as f:
                raw += "\n" + f.read()
        self.assertNotIn("FATAL", raw)

        self.assertIn("gpu vectorAdd ASYNC correct=256/256", raw,
            "ColdFire async vectorAdd not bit-exact; the posted thread-sync path "
            "must match the synchronous result")
        self.assertIn("async offload completed", raw,
            "Quetz async engine did not retire the posted offload")

        stats = parse_stats(sst_outfile)
        self.assertEqual(stats.get("cpu.async_submits.0", 0), 1,
            "expected exactly one posted thread-sync")
        self.assertGreater(stats.get("cpu.async_overlap_cycles.0", 0), 0,
            "ColdFire core must overlap with the posted offload")


class testcase_quetz_p6_usermode(SSTTestCase):
    """P6: user-mode synchronous MMIO via the linux-user SIGSEGV trap.

    User mode has no QEMU device map, so the doorbell aperture is reserved
    PROT_NONE (-sst-mmio-range) and faulting loads/stores route to the same sync
    mailbox the system-mode bridge uses. These need an overlay-built qemu-<arch>
    (apply-qemu-overlay.sh); they skip gracefully where it is unavailable.
    """

    def setUp(self):
        super(type(self), self).setUp()

    def tearDown(self):
        super(type(self), self).tearDown()

    def _qemu(self, target):
        import shutil
        sst_bindir = sstsimulator_conf_get_value("SSTCore", "bindir", str, "")
        qemu_bin = os.path.join(sst_bindir, target)
        if not os.path.exists(qemu_bin):
            found = shutil.which(target)
            if found:
                qemu_bin = found
        return qemu_bin if os.path.exists(qemu_bin) else None

    def _require_sst_mmio(self, qemu_bin, target):
        if qemu_bin is None:
            self.skipTest("{} not found; skipping P6 user-mode test".format(target))
        try:
            out = subprocess.run([qemu_bin, "-h"], capture_output=True,
                                 text=True, timeout=30)
            blob = (out.stdout or "") + (out.stderr or "")
        except Exception:
            blob = ""
        if "sst-mmio-range" not in blob:
            self.skipTest("{} lacks -sst-mmio-range (rebuild via "
                          "qemu-overlay/apply-qemu-overlay.sh); skipping".format(target))

    def _common_env(self, qemu_bin, exe_abs):
        sst_prefix, _, sst_libexec = sst_paths()
        os.environ["SST_HOME"] = sst_prefix
        os.environ["QUETZ_QEMU"] = qemu_bin
        os.environ["QUETZ_EXE"] = exe_abs
        os.environ["QUETZ_PLUGIN"] = os.path.join(
            sst_libexec, "libqemu_sst_plugin.so")
        os.environ["QUETZ_RAM_START"] = "0"
        os.environ["QUETZ_RAM_END"] = "0xFFFFFFFF"
        enable_mmio_payload_delivery()

    # --- synthetic-GPU-device round-trips (no GPGPU-Sim needed) --------------
    def _run_roundtrip(self, target, binary, mmio_base, qemu_args):
        qemu_bin = self._qemu(target)
        self._require_sst_mmio(qemu_bin, target)
        test_path = self.get_testsuite_dir()
        exe_abs = os.path.join(test_path, "binaries", binary)
        if not os.path.exists(exe_abs):
            self.skipTest("{} not built; run usermode/sources/build.sh".format(binary))

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "quetz_p6_tests", target + "_roundtrip")
        os.makedirs(outdir, exist_ok=True)
        sdlfile = os.path.join(test_path, "usermode", "basic_quetz_gpu_mmio.py")
        out = os.path.join(outdir, "rt.out")
        err = os.path.join(outdir, "rt.err")
        mpi = os.path.join(outdir, "rt.testfile")

        self._common_env(qemu_bin, exe_abs)
        os.environ["QUETZ_QEMU_ARGS"] = qemu_args
        os.environ["QUETZ_MMIO_START"] = hex(mmio_base)
        os.environ["QUETZ_MMIO_END"] = hex(mmio_base + 0x3FF)

        self.run_sst(sdlfile, out, err, mpi_out_files=mpi,
                     set_cwd=outdir, timeout_sec=180)
        with open(out) as f:
            raw = f.read()
        self.assertNotIn("FATAL", raw)
        self.assertIn("roundtrip_done", raw,
            "guest did not complete the MMIO round-trip")
        stats = parse_stats(out)
        self.assertGreaterEqual(stats.get("cpu.mmio_write_requests.0", 0), 1,
            "doorbell store must reach the sync mailbox")
        self.assertGreaterEqual(stats.get("cpu.mmio_read_requests.0", 0), 2,
            "STATUS + KERNEL_ID loads must reach the sync mailbox")

    def test_quetz_p6_roundtrip_riscv(self):
        self._run_roundtrip("qemu-riscv64", "rv64_mmio_roundtrip",
                            0x80100000, "-R 0x100000000")

    def test_quetz_p6_roundtrip_m68k(self):
        self._run_roundtrip("qemu-m68k", "m68k_mmio_roundtrip", 0x70000000, "")

    # --- balar/GPGPU-Sim vectorAdd offloads ----------------------------------
    def _run_balar(self, target, binary, qemu_args, async_offload=False):
        if not os.environ.get("GPGPUSIM_ROOT"):
            self.skipTest("GPGPUSIM_ROOT not set; skipping P6 balar test")
        qemu_bin = self._qemu(target)
        self._require_sst_mmio(qemu_bin, target)
        test_path = self.get_testsuite_dir()
        exe_abs = os.path.join(test_path, "binaries", binary)
        balar_tests = os.path.normpath(
            os.path.join(test_path, "..", "..", "balar", "tests"))
        cfg_file = os.path.join(balar_tests, "gpu-v100-mem.cfg")
        cuda_exe = os.path.join(balar_tests, "balar_trace", "vectorAdd")
        gpgpusim_cfg = os.path.join(balar_tests, "gpgpusim.config")
        for p, what in [(exe_abs, binary), (cfg_file, "balar cfg"),
                        (cuda_exe, "vectorAdd"), (gpgpusim_cfg, "gpgpusim.config")]:
            if not os.path.exists(p):
                self.skipTest("{} not found at {}; skipping".format(what, p))

        outdir = os.path.join(self.get_test_output_run_dir(),
                              "quetz_p6_tests", target + "_balar"
                              + ("_async" if async_offload else ""))
        os.makedirs(outdir, exist_ok=True)
        import shutil as _shutil
        _shutil.copy(gpgpusim_cfg, os.path.join(outdir, "gpgpusim.config"))

        sdlfile = os.path.join(test_path, "usermode", "basic_quetz_balar.py")
        out = os.path.join(outdir, "balar.out")
        err = os.path.join(outdir, "balar.err")
        mpi = os.path.join(outdir, "balar.testfile")

        self._common_env(qemu_bin, exe_abs)
        os.environ["QUETZ_QEMU_ARGS"] = qemu_args
        os.environ["QUETZ_MMIO_START"] = "0x70000000"
        os.environ["QUETZ_MMIO_END"] = "0x700005FF"
        os.environ["BALAR_CONFIG"] = cfg_file
        os.environ["BALAR_CUDA_EXE_PATH"] = cuda_exe
        if async_offload:
            os.environ["QUETZ_ASYNC_OFFLOAD"] = "1"
        else:
            os.environ.pop("QUETZ_ASYNC_OFFLOAD", None)

        self.run_sst(sdlfile, out, err, mpi_out_files=mpi,
                     set_cwd=outdir, timeout_sec=600)
        os.environ.pop("QUETZ_ASYNC_OFFLOAD", None)

        with open(out) as f:
            raw = f.read()
        self.assertNotIn("FATAL", raw)
        self.assertIn("correct=256/256", raw,
            "user-mode balar vectorAdd not bit-exact")
        if async_offload:
            stats = parse_stats(out)
            self.assertEqual(stats.get("cpu.async_submits.0", 0), 1,
                "expected one posted thread-sync")
            self.assertGreater(stats.get("cpu.async_overlap_cycles.0", 0), 0,
                "core must overlap with the posted offload")

    def test_quetz_p6_balar_riscv(self):
        self._run_balar("qemu-riscv64", "rv64_balar_user", "-R 0x100000000")

    def test_quetz_p6_balar_m68k(self):
        self._run_balar("qemu-m68k", "m68k_balar_user", "")

    def test_quetz_p6_balar_async_m68k(self):
        self._run_balar("qemu-m68k", "m68k_balar_async_user", "",
                        async_offload=True)
