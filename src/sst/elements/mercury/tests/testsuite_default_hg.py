# -*- coding: utf-8 -*-
import os
import subprocess

from sst_unittest import *
from sst_unittest_support import *

################################################################################

class testcase_hg(SSTTestCase):

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

    @unittest.skipIf(testing_check_get_num_ranks() > 1, "hooks not tested with more than one rank")
    def test_hooks_free(self):
        self.hooks_template("hooks_free", "--mode free")

    @unittest.skipIf(testing_check_get_num_ranks() > 1, "hooks not tested with more than one rank")
    def test_hooks_compute(self):
        self.hooks_template("hooks_compute", "--mode compute")

    @unittest.skipIf(testing_check_get_num_ranks() > 1, "hooks not tested with more than one rank")
    def test_hooks_compute_zero_threads(self):
        self.hooks_template("hooks_compute_zero_threads", "--mode compute-zero",
                            expected_error="thread count 0")

    @unittest.skipIf(testing_check_get_num_ranks() > 1, "hooks not tested with more than one rank")
    def test_hooks_compute_static_init(self):
        self.hooks_template("hooks_compute_static_init", "--mode static-init",
                            expected_error="called outside a running Mercury application thread")

    @unittest.skipIf(testing_check_get_num_ranks() > 1, "hooks not tested with more than one rank")
    def test_hooks_advance(self):
        self.hooks_template("hooks_advance", "--mode advance --param ov=19ns")

    @unittest.skipIf(testing_check_get_num_ranks() > 1, "hooks not tested with more than one rank")
    def test_hooks_advance_missing(self):
        self.hooks_template("hooks_advance_missing", "--mode advance",
                            expected_error="app 1 has no parameter 'ov'; set app1.ov")

    @unittest.skipIf(testing_check_get_num_ranks() > 1, "hooks not tested with more than one rank")
    def test_hooks_advance_bad_unit(self):
        self.hooks_template("hooks_advance_bad_unit", "--mode advance --param ov=10usec",
                            expected_error="is not a time: ")

    @unittest.skipIf(testing_check_get_num_ranks() > 1, "hooks not tested with more than one rank")
    def test_hooks_advance_not_time(self):
        self.hooks_template("hooks_advance_not_time", "--mode advance --param ov=10Hz",
                            expected_error="is not a time; give it units of seconds")

    @unittest.skipIf(testing_check_get_num_ranks() > 1, "hooks not tested with more than one rank")
    def test_hooks_advance_negative_zero(self):
        self.hooks_template("hooks_advance_negative_zero", "--mode advance --param ov=-0us")

    @unittest.skipIf(testing_check_get_num_ranks() > 1, "hooks not tested with more than one rank")
    def test_hooks_advance_negative(self):
        self.hooks_template("hooks_advance_negative", "--mode advance --param ov=-5us",
                            expected_error="is negative")

#####

    def hooks_template(self, testcase, model_args, expected_error=None):
        # Runs tests/hooks.py. With expected_error, the run must fail with
        # that message; otherwise its output must match the reference file.
        test_path = self.get_testsuite_dir()
        outdir = self.get_test_output_run_dir()
        tmpdir = self.get_test_output_tmp_dir()
        sdlfile = "{0}/hooks.py".format(test_path)
        reffile = "{0}/refFiles/{1}.out".format(test_path, testcase)
        outfile = "{0}/{1}.out".format(outdir, testcase)
        errfile = "{0}/{1}.err".format(outdir, testcase)
        cmpfile = "{0}/{1}.cmp".format(tmpdir, testcase)
        other_args = '--model-options="{0}"'.format(model_args)
        if expected_error is not None:
            self.run_sst(sdlfile, outfile, errfile, set_cwd=test_path,
                         other_args=other_args, expected_rc=1)
            with open(outfile) as out, open(errfile) as err:
                self.assertIn(expected_error, out.read() + err.read())
            return
        self.run_sst(sdlfile, outfile, errfile, set_cwd=test_path,
                     other_args=other_args)
        testing_remove_component_warning_from_file(outfile)
        os.system("cp {0} {1}".format(outfile, cmpfile))
        cmp_result = testing_compare_sorted_diff(testcase, cmpfile, reffile)
        if (cmp_result == False):
            diffdata = testing_get_diff_data(testcase)
            log_failure(diffdata)
        self.assertTrue(cmp_result, "Sorted Output file {0} does not match sorted Reference File {1}".format(cmpfile, reffile))

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
