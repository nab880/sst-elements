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

from quetz_test_helpers import (
    assert_class_balance,
    apply_usermode_region_handlers,
    compare_gold,
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
        log_debug("Quetz test #{} ({}): qemu={} with_l1={} isa={}".format(
            testnum, testname, qemu_target, with_l1, isa))
        self._quetz_test_template(testnum, testname, qemu_target, exe_rel,
                                   with_l1, isa, timeout_sec)

    # -------------------------------------------------------------------------
    def _quetz_test_template(self, testnum, testname, qemu_target, exe_rel,
                              with_l1, isa, testtimeout=120):
        test_path = self.get_testsuite_dir()   # .../quetz/tests/

        sst_prefix  = sstsimulator_conf_get_value("SSTCore", "prefix",     str, "")
        sst_bindir  = sstsimulator_conf_get_value("SSTCore", "bindir",     str, "")
        sst_libexec = sstsimulator_conf_get_value("SSTCore", "libexecdir", str, "")

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

        if os.path.exists(ref_outfile):
            cmp_result = compare_gold(testname, sst_outfile, ref_outfile,
                                      update_files=updateFiles)
            if not cmp_result:
                log_failure(oscmd)
            self.assertTrue(cmp_result,
                "Quetz output {} does not match reference {}".format(
                    sst_outfile, ref_outfile))
        else:
            log_testing_note(
                "Quetz test {} has no gold file; did not compare".format(testname))

    # -------------------------------------------------------------------------
    # Multicore halt-quorum test: verify both vCPUs ran to completion.
    # -------------------------------------------------------------------------
    def test_quetz_multicore_quorum(self):
        test_path = self.get_testsuite_dir()

        sst_prefix  = sstsimulator_conf_get_value("SSTCore", "prefix",     str, "")
        sst_bindir  = sstsimulator_conf_get_value("SSTCore", "bindir",     str, "")
        sst_libexec = sstsimulator_conf_get_value("SSTCore", "libexecdir", str, "")

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

        sst_prefix  = sstsimulator_conf_get_value("SSTCore", "prefix",     str, "")
        sst_bindir  = sstsimulator_conf_get_value("SSTCore", "bindir",     str, "")
        sst_libexec = sstsimulator_conf_get_value("SSTCore", "libexecdir", str, "")

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

        sst_prefix  = sstsimulator_conf_get_value("SSTCore", "prefix",     str, "")
        sst_bindir  = sstsimulator_conf_get_value("SSTCore", "bindir",     str, "")
        sst_libexec = sstsimulator_conf_get_value("SSTCore", "libexecdir", str, "")

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
        sst_prefix  = sstsimulator_conf_get_value("SSTCore", "prefix",     str, "")
        sst_bindir  = sstsimulator_conf_get_value("SSTCore", "bindir",     str, "")
        sst_libexec = sstsimulator_conf_get_value("SSTCore", "libexecdir", str, "")

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
        sst_prefix  = sstsimulator_conf_get_value("SSTCore", "prefix",     str, "")
        sst_bindir  = sstsimulator_conf_get_value("SSTCore", "bindir",     str, "")
        sst_libexec = sstsimulator_conf_get_value("SSTCore", "libexecdir", str, "")

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
        sst_prefix  = sstsimulator_conf_get_value("SSTCore", "prefix",     str, "")
        sst_bindir  = sstsimulator_conf_get_value("SSTCore", "bindir",     str, "")
        sst_libexec = sstsimulator_conf_get_value("SSTCore", "libexecdir", str, "")

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
        sst_prefix  = sstsimulator_conf_get_value("SSTCore", "prefix",     str, "")
        sst_bindir  = sstsimulator_conf_get_value("SSTCore", "bindir",     str, "")
        sst_libexec = sstsimulator_conf_get_value("SSTCore", "libexecdir", str, "")

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
        sst_prefix  = sstsimulator_conf_get_value("SSTCore", "prefix",     str, "")
        sst_bindir  = sstsimulator_conf_get_value("SSTCore", "bindir",     str, "")
        sst_libexec = sstsimulator_conf_get_value("SSTCore", "libexecdir", str, "")

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

        sst_prefix  = sstsimulator_conf_get_value("SSTCore", "prefix",     str, "")
        sst_bindir  = sstsimulator_conf_get_value("SSTCore", "bindir",     str, "")
        sst_libexec = sstsimulator_conf_get_value("SSTCore", "libexecdir", str, "")

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

        if os.path.exists(ref_outfile):
            cmp_result = compare_gold(testname, sst_outfile, ref_outfile,
                                      update_files=updateFiles)
            self.assertTrue(cmp_result,
                "Quetz sysmode output {} does not match reference {}".format(
                    sst_outfile, ref_outfile))
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
        sst_prefix  = sstsimulator_conf_get_value("SSTCore", "prefix",     str, "")
        sst_bindir  = sstsimulator_conf_get_value("SSTCore", "bindir",     str, "")
        sst_libexec = sstsimulator_conf_get_value("SSTCore", "libexecdir", str, "")

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

        if os.path.exists(ref_outfile):
            cmp_result = compare_gold(testname, sst_outfile, ref_outfile,
                                      update_files=updateFiles)
            self.assertTrue(cmp_result,
                "Quetz preset sysmode output {} does not match reference {} "
                "(platform preset should yield equivalent stats to explicit "
                "region handler params)".format(sst_outfile, ref_outfile))
        else:
            log_testing_note(
                "No gold file at {}; preset test ran but was not compared".format(
                    ref_outfile))

    # -------------------------------------------------------------------------
    def test_quetz_sysmode_filtered_only(self):
        test_path = self.get_testsuite_dir()
        sst_prefix  = sstsimulator_conf_get_value("SSTCore", "prefix",     str, "")
        sst_bindir  = sstsimulator_conf_get_value("SSTCore", "bindir",     str, "")
        sst_libexec = sstsimulator_conf_get_value("SSTCore", "libexecdir", str, "")

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
        sst_prefix  = sstsimulator_conf_get_value("SSTCore", "prefix",     str, "")
        sst_bindir  = sstsimulator_conf_get_value("SSTCore", "bindir",     str, "")
        sst_libexec = sstsimulator_conf_get_value("SSTCore", "libexecdir", str, "")

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

        memmaps = [("sub_ram", 0x00000000, 0x7FFFFFFF, "filtered")]
        make_sysmode_env(sst_prefix, sst_libexec, qemu_bin, exe_abs,
                         "-machine virt -nographic -bios none",
                         "-kernel", 0, 0xFFFFFFFF, memmaps)
        os.environ["QUETZ_MMIO_START"] = "0x80100000"
        os.environ["QUETZ_MMIO_END"]   = "0x801003FF"
        os.environ["QUETZ_REGION_HANDLER_COUNT"] = "1"

        self.run_sst(sdlfile, sst_outfile, sst_errfile,
                     mpi_out_files=mpifiles, set_cwd=outdir, timeout_sec=120)

        stats = parse_stats(sst_outfile)
        mmio_writes = stats.get("cpu.mmio_write_requests.0", 0)
        mmio_reads  = stats.get("cpu.mmio_read_requests.0", 0)
        cache_writes = stats.get("cpu.write_requests.0", 0)
        cache_reads  = stats.get("cpu.read_requests.0", 0)

        self.assertGreaterEqual(mmio_writes, 1,
            "doorbell write should appear on mmio_write_requests")
        self.assertGreaterEqual(mmio_reads, 1,
            "status read should appear on mmio_read_requests")
        self.assertEqual(cache_writes, 0,
            "MMIO poke firmware should not forward writes on cache_link "
            "(UART/testdev are filtered; doorbell uses mmio_link)")
        self.assertEqual(cache_reads, 0,
            "MMIO poke firmware should not forward reads on cache_link")

    # -------------------------------------------------------------------------
    def test_quetz_sysmode_gpu_kernel(self):
        """QuetzGpuDevice: doorbell launches kernels with timed BUSY/IDLE."""
        test_path = self.get_testsuite_dir()
        sst_prefix  = sstsimulator_conf_get_value("SSTCore", "prefix",     str, "")
        sst_bindir  = sstsimulator_conf_get_value("SSTCore", "bindir",     str, "")
        sst_libexec = sstsimulator_conf_get_value("SSTCore", "libexecdir", str, "")

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
        os.environ["QUETZ_REGION_HANDLER_COUNT"] = "2"

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
        self.assertGreaterEqual(mmio_reads, 6,
            "expected status polls + KERNEL_ID reads")
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
            "cooperative firmware should not queue doorbells while BUSY")

    # -------------------------------------------------------------------------
    def test_quetz_usermode_gpu_kernel(self):
        """User-mode QuetzGpuDevice: mmio_link doorbell + timed BUSY/IDLE."""
        test_path = self.get_testsuite_dir()
        sst_prefix  = sstsimulator_conf_get_value("SSTCore", "prefix",     str, "")
        sst_bindir  = sstsimulator_conf_get_value("SSTCore", "bindir",     str, "")
        sst_libexec = sstsimulator_conf_get_value("SSTCore", "libexecdir", str, "")

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
        if not os.path.exists(exe_abs):
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
            "guest launches exactly three kernels")
        self.assertIsNotNone(busy_cycles,
            "gpu.busy_cycles not found in output")
        self.assertGreater(busy_cycles, 0,
            "GPU should accumulate busy_cycles during kernel runs")

    # -------------------------------------------------------------------------
    def test_quetz_usermode_gpu_trace_capture(self):
        """User-mode GpuTraceRegionHandler: doorbell + status poll capture."""
        test_path = self.get_testsuite_dir()
        sst_prefix  = sstsimulator_conf_get_value("SSTCore", "prefix",     str, "")
        sst_bindir  = sstsimulator_conf_get_value("SSTCore", "bindir",     str, "")
        sst_libexec = sstsimulator_conf_get_value("SSTCore", "libexecdir", str, "")

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
        if not os.path.exists(exe_abs):
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
        sst_prefix  = sstsimulator_conf_get_value("SSTCore", "prefix",     str, "")
        sst_bindir  = sstsimulator_conf_get_value("SSTCore", "bindir",     str, "")
        sst_libexec = sstsimulator_conf_get_value("SSTCore", "libexecdir", str, "")

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
        sst_prefix  = sstsimulator_conf_get_value("SSTCore", "prefix",     str, "")
        sst_bindir  = sstsimulator_conf_get_value("SSTCore", "bindir",     str, "")
        sst_libexec = sstsimulator_conf_get_value("SSTCore", "libexecdir", str, "")

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
