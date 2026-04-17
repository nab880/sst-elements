# -*- coding: utf-8 -*-

from sst_unittest import *
from sst_unittest_support import *

import os


class testcase_carcosa(SSTTestCase):

    def setUp(self):
        super(type(self), self).setUp()

    def tearDown(self):
        super(type(self), self).tearDown()

    def test_carcosa_corruptmem_basic(self):
        self._run_refdiff("testCorruptMemBasic.py", "test_carcosa_corruptmem_basic")

    def test_carcosa_corruptmem_double(self):
        self._run_refdiff("testCorruptMemDouble.py", "test_carcosa_corruptmem_double")

    def test_carcosa_corruptmem_double_overlap(self):
        self._run_refdiff("testCorruptMemDoubleOverlap.py", "test_carcosa_corruptmem_double_overlap")

    def test_carcosa_random_drop(self):
        self._run_smoke("testRandomDrop.py", "test_carcosa_random_drop")

    def test_carcosa_random_flip(self):
        self._run_smoke("testRandomFlip.py", "test_carcosa_random_flip")

    def test_carcosa_hali_memh(self):
        self._run_refdiff("testHaliMemH.py", "test_carcosa_hali_memh")

    def test_carcosa_hali_backing(self):
        self._run_refdiff("testHaliBacking.py", "test_carcosa_hali_backing")

    def test_carcosa_hali_pm(self):
        self._run_refdiff("testHaliPM.py", "test_carcosa_hali_pm")

    def test_carcosa_manager_logic(self):
        self._run_refdiff("testManagerLogic.py", "test_carcosa_manager_logic")

    def test_carcosa_dynamic_pm(self):
        self._run_refdiff("testDynamicPM.py", "test_carcosa_dynamic_pm")

    def test_carcosa_pingpong(self):
        self._run_refdiff("testCarcosaPingPong.py", "test_carcosa_pingpong", testtimeout=600)

    @unittest.skip("Quarantined: Vanadis/Hali StuckAt interface + bitset parser pending")
    def test_carcosa_stuckat_basic(self):
        self._run_refdiff("testStuckAtBasic.py", "test_carcosa_stuckat_basic")

    @unittest.skip("Quarantined: Vanadis/Hali StuckAt interface + bitset parser pending")
    def test_carcosa_stuckat_multiple(self):
        self._run_refdiff("testStuckAtMultiple.py", "test_carcosa_stuckat_multiple")

    @unittest.skip("Quarantined: Vanadis/Hali StuckAt interface + bitset parser pending")
    def test_carcosa_stuckat_overlap(self):
        self._run_refdiff("testStuckAtOverlap.py", "test_carcosa_stuckat_overlap")

    @unittest.skip("Quarantined: Vanadis/Hali StuckAt interface + bitset parser pending")
    def test_carcosa_stuckat_samebyte(self):
        self._run_refdiff("testStuckAtSameByte.py", "test_carcosa_stuckat_samebyte")

    _ignore_lines = [
        "WARNING: No components are assigned to",
        "Notice: memory controller's region is larger than the backend's mem_size",
        "Region: start=",
        "highlink_=0x",
        "lowlink_=0x",
        "[FaultInjectorMemH]",
    ]

    def _paths(self, sdlname, testname):
        test_path = self.get_testsuite_dir()
        outdir = self.get_test_output_run_dir()
        sdlfile = "{0}/{1}".format(test_path, sdlname)
        outfile = "{0}/{1}.out".format(outdir, testname)
        errfile = "{0}/{1}.err".format(outdir, testname)
        mpioutfiles = "{0}/{1}.testfile".format(outdir, testname)
        reffile = "{0}/refFiles/{1}.out".format(test_path, testname)
        return sdlfile, outfile, errfile, mpioutfiles, reffile, test_path

    def _run_smoke(self, sdlname, testname, testtimeout=300):
        sdlfile, outfile, errfile, mpioutfiles, _reffile, test_path = self._paths(sdlname, testname)

        self.run_sst(sdlfile, outfile, errfile,
                     mpi_out_files=mpioutfiles, set_cwd=test_path, timeout_sec=testtimeout)

        if os_test_file(errfile, "-s"):
            log_testing_note("Carcosa {0} has a Non-Empty Error File {1}".format(testname, errfile))

        grepstr = "Simulation is complete"
        found = False
        with open(outfile, "r") as f:
            for line in f:
                if grepstr in line:
                    found = True
                    break
        self.assertTrue(
            found,
            "Carcosa smoke test {0}: string '{1}' not found in output file {2}".format(
                testname, grepstr, outfile
            ),
        )

    def _run_refdiff(self, sdlname, testname, testtimeout=300):
        sdlfile, outfile, errfile, mpioutfiles, reffile, test_path = self._paths(sdlname, testname)

        self.run_sst(sdlfile, outfile, errfile,
                     mpi_out_files=mpioutfiles, set_cwd=test_path, timeout_sec=testtimeout)

        if os_test_file(errfile, "-s"):
            log_testing_note("Carcosa {0} has a Non-Empty Error File {1}".format(testname, errfile))

        filesAreTheSame, statDiffs, othDiffs = testing_stat_output_diff(
            outfile, reffile, self._ignore_lines, {}, True
        )

        if filesAreTheSame:
            log_debug(" -- Output file {0} passed check against the Reference File {1}".format(outfile, reffile))
        else:
            diffdata = self._prettyPrintDiffs(statDiffs, othDiffs)
            log_failure(diffdata)
            self.assertTrue(
                filesAreTheSame,
                "Carcosa refdiff test {0}: output file {1} does not match reference {2}".format(
                    testname, outfile, reffile
                ),
            )

    def _prettyPrintDiffs(self, stat_diff, oth_diff):
        out = ""
        if len(stat_diff) != 0:
            out = "Statistic diffs:\n"
            for x in stat_diff:
                out += (x[0] + " " + ",".join(str(y) for y in x[1:]) + "\n")
        if len(oth_diff) != 0:
            out += "Non-statistic diffs:\n"
            for x in oth_diff:
                out += x[0] + " " + x[1] + "\n"
        return out
