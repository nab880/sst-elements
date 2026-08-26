# -*- coding: utf-8 -*-
import os
import subprocess
from pathlib import Path

from sst_unittest import *
from sst_unittest_support import *

################################################################################

class testcase_mask_mpi(SSTTestCase):

    def setUp(self):
        super(testcase_mask_mpi, self).setUp()
        global module_init
        # Put test based setup code here. it is called once before every test

    def tearDown(self):
        # Put test based teardown code here. it is called once after every test
        super(testcase_mask_mpi, self).tearDown()

#####

    def test_sendrecv(self):
        self.mask_mpi_template("test_sendrecv")

    def test_reduce(self):
        self.mask_mpi_template("test_reduce")

    def test_alltoall(self):
        self.mask_mpi_template("test_alltoall")

    def test_allgather(self):
        self.mask_mpi_template("test_allgather")

    def test_halo3d26(self):
        self.mask_mpi_template("test_halo3d26")

    def test_sendrecv_multi_vn(self):
        self.mask_mpi_template("test_sendrecv", model_option="multi-vn", normalize_time=True)

    def test_reduce_multi_vn(self):
        self.mask_mpi_template("test_reduce", model_option="multi-vn", normalize_time=True)

    def test_alltoall_multi_vn(self):
        self.mask_mpi_template("test_alltoall", model_option="multi-vn", normalize_time=True)

    def test_allgather_multi_vn(self):
        self.mask_mpi_template("test_allgather", model_option="multi-vn", normalize_time=True)

    def test_halo3d26_multi_vn(self):
        self.mask_mpi_template("test_halo3d26", model_option="multi-vn", normalize_time=True)

    def test_sendrecv_multi_vn_fragmented(self):
        self.mask_mpi_template(
            "test_sendrecv", model_option="multi-vn-fragmented", normalize_time=True)

    def test_sendrecv_configured_ordinary_vn(self):
        self.mask_mpi_template(
            "test_sendrecv", model_option="multi-vn-observable", normalize_time=True)

    def test_invalid_mercury_vn_roles(self):
        test_path = self.get_testsuite_dir()
        outdir = self.get_test_output_run_dir()
        sdlfile = f"{test_path}/test_sendrecv.py"
        cases = {
            "bad-vn-count": "Mercury num_vns must be positive",
            "bad-vn-range": "Mercury rejected VN configuration",
            "bad-vn-partial-service": "Mercury rejected VN configuration",
            "bad-vn-duplicate-service": "Mercury rejected VN configuration",
            "bad-vn-native-alias": "Mercury rejected VN configuration",
            "bad-vn-manager-service-alias": "Mercury rejected VN configuration",
        }
        for mode, diagnostic in cases.items():
            with self.subTest(mode=mode):
                outfile = f"{outdir}/test_sendrecv_{mode}.out"
                errfile = f"{outdir}/test_sendrecv_{mode}.err"
                self.run_sst(sdlfile, outfile, errfile,
                    other_args=f'--model-options="{mode}"', expected_rc=1,
                    timeout_sec=5, set_cwd=test_path)
                combined = Path(outfile).read_text(encoding="utf-8") + \
                    Path(errfile).read_text(encoding="utf-8")
                self.assertIn(diagnostic, combined)
                self.assertNotIn("Simulation is complete", combined)

#####

    def mask_mpi_template(self, testcase, striptotail=0, model_option=None, normalize_time=False):
        # Get the path to the test files
        test_path = self.get_testsuite_dir()
        outdir = self.get_test_output_run_dir()
        tmpdir = self.get_test_output_tmp_dir()

        # Set the various file paths
        testDataFileName="{0}".format(testcase)

        sdlfile = "{0}/{1}.py".format(test_path, testDataFileName)
        reffile = "{0}/refFiles/{1}.out".format(test_path, testDataFileName)
        run_name = testDataFileName if model_option is None else f"{testDataFileName}_{model_option}"
        outfile = "{0}/{1}.out".format(outdir, run_name)
        tmpfile = "{0}/{1}.tmp".format(tmpdir, run_name)
        cmpfile = "{0}/{1}.cmp".format(tmpdir, run_name)
        errfile = "{0}/{1}.err".format(outdir, run_name)
        mpioutfiles = "{0}/{1}.testfile".format(outdir, run_name)

        if model_option is None:
            self.run_sst(sdlfile, outfile, errfile, mpi_out_files=mpioutfiles, set_cwd=test_path)
        else:
            self.run_sst(sdlfile, outfile, errfile, mpi_out_files=mpioutfiles,
                set_cwd=test_path, other_args=f'--model-options="{model_option}"')

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

        if model_option is not None:
            self.assertFalse(os_test_file(errfile, "-s"),
                f"Multi-VN Mercury regression produced stderr: {errfile}")

        if normalize_time:
            actual_lines = Path(outfile).read_text(encoding="utf-8").splitlines()
            reference_lines = Path(reffile).read_text(encoding="utf-8").splitlines()
            self.assertEqual(1,
                sum(line.startswith("Simulation is complete") for line in actual_lines),
                "Multi-VN regression did not complete exactly once")
            actual_semantics = sorted(
                line for line in actual_lines if not line.startswith("Simulation is complete"))
            reference_semantics = sorted(
                line for line in reference_lines if not line.startswith("Simulation is complete"))
            self.assertEqual(reference_semantics, actual_semantics,
                "Multi-VN output differs from the legacy semantic result")
            return

        cmp_result = testing_compare_sorted_diff(testcase, cmpfile, reffile)
        if (cmp_result == False):
            diffdata = testing_get_diff_data(testcase)
            log_failure(diffdata)
        self.assertTrue(cmp_result, "Sorted Output file {0} does not match sorted Reference File {1}".format(cmpfile, reffile))
