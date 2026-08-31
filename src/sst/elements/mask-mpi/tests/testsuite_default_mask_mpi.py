from pathlib import Path

from sst_unittest import *
from sst_unittest_support import *


class testcase_mask_mpi(SSTTestCase):

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

    def test_native_multi_vn(self):
        self.mask_mpi_template("test_sendrecv", "multi-vn")

    def test_invalid_mercury_vn_roles(self):
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
                self.assert_model_fails("test_sendrecv.py", mode, diagnostic)


    def assert_model_fails(self, model, mode, diagnostic):
        test_dir = self.get_testsuite_dir()
        out_dir = self.get_test_output_run_dir()
        stem = f"{Path(model).stem}_{mode}"
        output, error = f"{out_dir}/{stem}.out", f"{out_dir}/{stem}.err"
        self.run_sst(f"{test_dir}/{model}", output, error, set_cwd=test_dir,
            other_args=f'--model-options="{mode}"', expected_rc=1, timeout_sec=5)
        combined = Path(output).read_text(encoding="utf-8") + \
                   Path(error).read_text(encoding="utf-8")
        self.assertIn(diagnostic, combined)

    def mask_mpi_template(self, testcase, mode=None):
        test_dir = self.get_testsuite_dir()
        out_dir = self.get_test_output_run_dir()
        run_name = testcase if mode is None else f"{testcase}_{mode}"
        output, error = f"{out_dir}/{run_name}.out", f"{out_dir}/{run_name}.err"
        args = "" if mode is None else f'--model-options="{mode}"'
        self.run_sst(f"{test_dir}/{testcase}.py", output, error,
            mpi_out_files=f"{out_dir}/{run_name}.testfile", set_cwd=test_dir,
            other_args=args)
        testing_remove_component_warning_from_file(output)
        reference = f"{test_dir}/refFiles/{testcase}.out"
        if mode is None:
            self.assertTrue(testing_compare_sorted_diff(testcase, output, reference))
            return
        self.assertFalse(os_test_file(error, "-s"), "multi-VN run produced stderr")
        semantic = lambda path: sorted(
            line for line in Path(path).read_text(encoding="utf-8").splitlines()
            if not line.startswith("Simulation is complete"))
        self.assertEqual(semantic(reference), semantic(output))
