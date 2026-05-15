# -*- coding: utf-8 -*-
"""
testsuite_default_quetz.py — SST test harness for the Quetz element.

Tests run the basic_quetz.py SDL file and compare deterministic statistics
(instruction counts, request counts) against reference files:
  usermode/small/<testname>/sst.stdout.gold

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

module_init = 0
module_sema = threading.Semaphore()
quetz_test_matrix = []

updateFiles = False
# updateFiles = True   # uncomment to regenerate gold files

# ---------------------------------------------------------------------------
# Filter: keep only " cpu." event-count lines; discard timing stats.
# ---------------------------------------------------------------------------
_TIMING_KEYWORDS = ("_latency", ".cycles.", "active_cycles", "stall_cycles")

class QuetzStatsFilter(LineFilter):
    """Keep only deterministic event-count statistics from QuetzComponent."""
    def filter(self, line):
        if not line.startswith(" cpu."):
            return None
        for kw in _TIMING_KEYWORDS:
            if kw in line:
                return None
        return line

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

        os.environ["QUETZ_EXE"]     = exe_abs
        os.environ["QUETZ_QEMU"]    = qemu_bin
        os.environ["QUETZ_PLUGIN"]  = os.path.join(sst_libexec,
                                                    "libqemu_sst_plugin.so")
        os.environ["QUETZ_WITH_L1"] = "1" if with_l1 else "0"
        os.environ["QUETZ_ISA"]     = isa
        os.environ["SST_HOME"]      = sst_prefix

        oscmd = self.run_sst(sdlfile, sst_outfile, sst_errfile,
                             mpi_out_files=mpifiles,
                             set_cwd=outdir,
                             timeout_sec=testtimeout)

        if os.path.exists(ref_outfile):
            cmp_result = testing_compare_filtered_diff(
                testname, sst_outfile, ref_outfile,
                filters=[QuetzStatsFilter()])
            if not cmp_result:
                diffdata = testing_get_diff_data(testname)
                log_failure(oscmd)
                log_failure(diffdata)
                if updateFiles:
                    import subprocess
                    print("Updating gold file", sst_outfile, "->", ref_outfile)
                    subprocess.call(["cp", sst_outfile, ref_outfile])
            self.assertTrue(cmp_result,
                "Quetz output {} does not match reference {}".format(
                    sst_outfile, ref_outfile))
        else:
            log_testing_note(
                "Quetz test {} has no gold file; did not compare".format(testname))
