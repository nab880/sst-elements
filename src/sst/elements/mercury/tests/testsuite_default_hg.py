from pathlib import Path

from sst_unittest import *
from sst_unittest_support import *


class testcase_hg(SSTTestCase):

    @unittest.skipIf(testing_check_get_num_threads() > 1, "ostest requires one thread")
    @unittest.skipIf(testing_check_get_num_ranks() > 1, "ostest requires one rank")
    def test_os(self):
        self.simple_components_template("ostest")

    @unittest.skipIf(testing_check_get_num_threads() > 1, "ostest-nano requires one thread")
    @unittest.skipIf(testing_check_get_num_ranks() > 1, "ostest-nano requires one rank")
    def test_os_nano(self):
        self.simple_components_template("ostest-nano")

    def test_mercury_network_service_tag_first(self):
        test_dir = self.get_testsuite_dir()
        out_dir = self.get_test_output_run_dir()
        output = f"{out_dir}/network_service_tag_first.out"
        error = f"{out_dir}/network_service_tag_first.err"
        self.run_sst(f"{test_dir}/network_service_tag_first.py", output, error,
            expected_rc=1, timeout_sec=5)
        combined = Path(output).read_text(encoding="utf-8") + \
                   Path(error).read_text(encoding="utf-8")
        self.assertIn("Mercury received unsupported network service 32768 on VN 0", combined)
        self.assertNotIn("couldn't cast event to NetworkMessage", combined)

    @unittest.skipIf(testing_check_get_num_threads() > 1, "manager VN smoke requires one thread")
    @unittest.skipIf(testing_check_get_num_ranks() > 1, "manager VN smoke requires one rank")
    def test_manager_vn_smoke(self):
        self.simple_components_template("manager_vn_smoke")

    def simple_components_template(self, testcase):
        test_dir = self.get_testsuite_dir()
        out_dir = self.get_test_output_run_dir()
        output = f"{out_dir}/{testcase}.out"
        error = f"{out_dir}/{testcase}.err"
        self.run_sst(f"{test_dir}/{testcase}.py", output, error,
            mpi_out_files=f"{out_dir}/{testcase}.testfile")
        testing_remove_component_warning_from_file(output)
        reference = f"{test_dir}/refFiles/{testcase}.out"
        self.assertTrue(testing_compare_sorted_diff(testcase, output, reference))
