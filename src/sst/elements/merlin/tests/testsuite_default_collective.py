import re
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

    def test_static_plan_contract(self):
        output, error = self.run_case("merlin_static_plan", "merlin_static_plan_contract.py")
        self.assertFalse(os_test_file(str(error), "-s"))
        self.assertIn("StaticCollectivePlan contract PASS", output.read_text(encoding="utf-8"))

    def test_static_transport_rejections(self):
        cases = (
            ("bad-flit", "invalid or unsupported static local projection"),
            ("bad-capacity", "invalid or unsupported static local projection"),
            ("bad-downstream-capacity", "unsupported by initialized downstream credits"),
            ("disconnected", "invalid or unsupported static local projection"),
        )
        for mode, diagnostic in cases:
            output, error = self.run_case(
                "merlin_static_" + mode, "merlin_static_ordinary_baseline.py", mode, 1)
            self.assertIn(diagnostic,
                          output.read_text(encoding="utf-8") + error.read_text(encoding="utf-8"))

    def test_static_service_disabled_baseline(self):
        disabled, disabled_error = self.run_case(
            "merlin_static_disabled", "merlin_static_ordinary_baseline.py")
        enabled, enabled_error = self.run_case(
            "merlin_static_enabled", "merlin_static_ordinary_baseline.py", "service")
        self.assertFalse(os_test_file(str(disabled_error), "-s"))
        self.assertFalse(os_test_file(str(enabled_error), "-s"))
        self.assertEqual(disabled.read_bytes(), enabled.read_bytes())
        stalls = [int(value) for value in re.findall(
            r"Nic [0-3] had ([0-9]+) stalled cycles", disabled.read_text(encoding="utf-8"))]
        self.assertEqual(4, len(stalls))
        self.assertTrue(all(value > 0 for value in stalls))
