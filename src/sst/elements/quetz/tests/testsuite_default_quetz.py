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
    compare_gold,
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

        stats = parse_stats(sst_outfile)

        needed = ["cpu.read_requests.0", "cpu.write_requests.0",
                  "cpu.int_compute.0", "cpu.fp_compute.0",
                  "cpu.vec_compute.0", "cpu.branch.0",
                  "cpu.instruction_count.0"]
        for s in needed:
            self.assertIn(s, stats,
                "Statistic {} not found in output".format(s))

        # instruction_count counts mem ops + NOPs.  Classified NOPs appear in
        # int/fp/vec/branch; unclassified (OTHER) NOPs only appear in no_ops.
        classified_nop = (stats["cpu.int_compute.0"] +
                          stats["cpu.fp_compute.0"] +
                          stats["cpu.vec_compute.0"] +
                          stats["cpu.branch.0"])
        other_nop = stats["cpu.no_ops.0"] - classified_nop
        class_sum = (stats["cpu.read_requests.0"] +
                     stats["cpu.write_requests.0"] +
                     classified_nop + other_nop)
        insn_count = stats["cpu.instruction_count.0"]

        self.assertEqual(class_sum, insn_count,
            "Class-balance identity failed: "
            "reads({}) + writes({}) + classified_nops({}) + other_nops({}) "
            "= {} != instruction_count({})".format(
                stats["cpu.read_requests.0"],
                stats["cpu.write_requests.0"],
                classified_nop, other_nop,
                class_sum, insn_count))

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
