# -*- coding: utf-8 -*-
import os
import re
import subprocess
from pathlib import Path

from sst_unittest import *
from sst_unittest_support import *

################################################################################

class testcase_hg(SSTTestCase):

    def test_mercury_collective_host_timing(self):
        test_path = Path(self.get_testsuite_dir())
        outdir = Path(self.get_test_output_run_dir())
        timings = {}
        for case in ("baseline", "memory", "submit", "completion", "contended", "unmodeled", "reentrant"):
            output = outdir / f"mercury_collective_host_{case}.out"
            error = outdir / f"mercury_collective_host_{case}.err"
            self.run_sst(str(test_path / "collective_host_timing.py"), str(output), str(error),
                other_args=f'--model-options="{case}"', timeout_sec=10)
            self.assertFalse(os_test_file(str(error), "-s"), f"Nonempty error file: {error}")
            text = output.read_text(encoding="utf-8")
            rows = re.findall(r"host timing rank=(\d) invocation=(\d) elapsed_ps=(\d+) in_place=PASS", text)
            self.assertEqual(4, len(rows), text)
            timings[case] = {(int(rank), int(invocation)): int(elapsed) for rank, invocation, elapsed in rows}
        # Two 8-byte transfers cost 2ns at 8GB/s and 200ns at 80MB/s.
        # The contended case queues behind a 400-byte transfer on the same channel.
        for case, expected_ps in (("memory", 198000), ("submit", 100000),
                                  ("completion", 100000), ("contended", 50000), ("unmodeled", -2000)):
            for key, baseline in timings["baseline"].items():
                self.assertAlmostEqual(expected_ps, timings[case][key] - baseline, delta=2,
                    msg=f"{case}: rank/invocation {key} bypassed the configured host cost")

    def test_mercury_collective_host_invalid_delay(self):
        test_path = Path(self.get_testsuite_dir())
        outdir = Path(self.get_test_output_run_dir())
        for case in ("negative_submit", "negative_completion", "overflow_submit", "overflow_completion"):
            output = outdir / f"mercury_collective_host_{case}.out"
            error = outdir / f"mercury_collective_host_{case}.err"
            self.run_sst(str(test_path / "collective_host_timing.py"), str(output), str(error),
                other_args=f'--model-options="{case}"', expected_rc=1, timeout_sec=10)
            text = output.read_text(encoding="utf-8") + error.read_text(encoding="utf-8")
            self.assertIn("delays must be nonnegative and fit the timebase", text)

    def setUp(self):
        super(testcase_hg, self).setUp()
        # Put test based setup code here. it is called once before every test

    def tearDown(self):
        # Put test based teardown code here. it is called once after every test
        super(testcase_hg, self).tearDown()

#####

    @unittest.skipIf(testing_check_get_num_threads() > 1, "ostest skipped if threads > 1 - single component in config")
    @unittest.skipIf(testing_check_get_num_ranks() > 1, "ostest skipped if ranks > 1 - single component in config")
    def test_os(self):
        self.simple_components_template("ostest")

    @unittest.skipIf(testing_check_get_num_threads() > 1, "ostest-nano skipped if threads > 1 - single component in config")
    @unittest.skipIf(testing_check_get_num_ranks() > 1, "ostest-nano skipped if ranks > 1 - single component in config")
    def test_os_nano(self):
        self.simple_components_template("ostest-nano")

    def test_mercury_network_service_tag_first(self):
        test_path = self.get_testsuite_dir()
        outdir = self.get_test_output_run_dir()
        output = f"{outdir}/network_service_tag_first.out"
        error = f"{outdir}/network_service_tag_first.err"

        self.run_sst(f"{test_path}/network_service_tag_first.py", output, error,
            expected_rc=1, timeout_sec=5)
        combined = Path(output).read_text(encoding="utf-8") + Path(error).read_text(encoding="utf-8")
        self.assertIn("Mercury received unsupported network service 32768 on VN 1", combined)
        self.assertNotIn("couldn't cast event to NetworkMessage", combined)
        self.assertNotIn("Bye!", combined)

    @unittest.skipIf(testing_check_get_num_threads() > 1, "manager VN smoke skipped if threads > 1")
    @unittest.skipIf(testing_check_get_num_ranks() > 1, "manager VN smoke skipped if ranks > 1")
    def test_manager_vn_smoke(self):
        self.simple_components_template("manager_vn_smoke")

#####

    def simple_components_template(self, testcase, striptotail=0):
        # Get the path to the test files
        test_path = self.get_testsuite_dir()
        outdir = self.get_test_output_run_dir()
        tmpdir = self.get_test_output_tmp_dir()

        # Set the various file paths
        testDataFileName="{0}".format(testcase)

        sdlfile = "{0}/{1}.py".format(test_path, testDataFileName)
        reffile = "{0}/refFiles/{1}.out".format(test_path, testDataFileName)
        outfile = "{0}/{1}.out".format(outdir, testDataFileName)
        tmpfile = "{0}/{1}.tmp".format(tmpdir, testDataFileName)
        cmpfile = "{0}/{1}.cmp".format(tmpdir, testDataFileName)
        errfile = "{0}/{1}.err".format(outdir, testDataFileName)
        mpioutfiles = "{0}/{1}.testfile".format(outdir, testDataFileName)

        self.run_sst(sdlfile, outfile, errfile, mpi_out_files=mpioutfiles)

        testing_remove_component_warning_from_file(outfile)

        # Copy the outfile to the cmpfile
        os.system("cp {0} {1}".format(outfile, cmpfile))

        if striptotail == 1:
            # Post processing of the output data to scrub it into a format to compare
            os.system("grep Random {0} > {1}".format(outfile, tmpfile))
            os.system("tail -5 {0} > {1}".format(tmpfile, cmpfile))

        # NOTE: THE PASS / FAIL EVALUATIONS ARE PORTED FROM THE SQE BAMBOO
        #       BASED testSuite_XXX.sh THESE SHOULD BE RE-EVALUATED BY THE
        #       DEVELOPER AGAINST THE LATEST VERSION OF SST TO SEE IF THE
        #       TESTS & RESULT FILES ARE STILL VALID

        # Perform the tests
        if os_test_file(errfile, "-s"):
            log_testing_note("hg test {0} has a Non-Empty Error File {1}".format(testDataFileName, errfile))

        cmp_result = testing_compare_sorted_diff(testcase, cmpfile, reffile)
        if (cmp_result == False):
            diffdata = testing_get_diff_data(testcase)
            log_failure(diffdata)
        self.assertTrue(cmp_result, "Sorted Output file {0} does not match sorted Reference File {1}".format(cmpfile, reffile))
