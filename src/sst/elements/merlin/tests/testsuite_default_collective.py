from pathlib import Path

from sst_unittest import *
from sst_unittest_support import *


class testcase_collective(SSTTestCase):

    def run_case(self, name, model, mode="", expected_rc=0):
        test_dir = self.get_testsuite_dir()
        out_dir = self.get_test_output_run_dir()
        output = Path(out_dir) / (name + ".out")
        error = Path(out_dir) / (name + ".err")
        args = '--model-options="%s"' % mode if mode else ""
        self.run_sst(str(Path(test_dir) / model), str(output), str(error),
                     other_args=args, expected_rc=expected_rc, timeout_sec=10)
        return output, error

    def test_collective_contract(self):
        output, error = self.run_case("collective_contract", "collective_contract.py")
        self.assertFalse(os_test_file(str(error), "-s"))
        self.assertIn("Merlin collective contract PASS", output.read_text(encoding="utf-8"))

    def test_network_service_contract(self):
        output, error = self.run_case("network_service_busy", "network_service_model.py", "busy")
        self.assertFalse(os_test_file(str(error), "-s"))
        self.assertIn("Merlin network-service integration PASS", output.read_text(encoding="utf-8"))
        for mode, diagnostic in (
                ("missing", "has no matching attached router processor"),
                ("multiple", "supports at most one service")):
            output, error = self.run_case(
                "network_service_" + mode, "network_service_model.py", mode, 1)
            self.assertIn(diagnostic,
                          output.read_text(encoding="utf-8") + error.read_text(encoding="utf-8"))
