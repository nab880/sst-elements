# -*- coding: utf-8 -*-

from pathlib import Path
import re

from sst_unittest import *
from sst_unittest_support import *


class testcase_collective(SSTTestCase):

    def test_collective_contract(self):
        test_dir = self.get_testsuite_dir()
        out_dir = self.get_test_output_run_dir()
        output = f"{out_dir}/collective_contract.out"
        error = f"{out_dir}/collective_contract.err"

        self.run_sst(f"{test_dir}/collective_contract.py", output, error)
        output_path = Path(output)
        output_path.write_text(
            "".join(
                line for line in output_path.read_text(encoding="utf-8").splitlines(keepends=True)
                if not line.startswith("WARNING: Building component")
            ),
            encoding="utf-8",
        )
        self.assertFalse(os_test_file(error, "-s"), f"Nonempty error file: {error}")
        reference = f"{test_dir}/refFiles/collective_contract.out"
        self.assertTrue(testing_compare_sorted_diff("collective_contract", output, reference))

    def test_collective_dependency_boundary(self):
        source_dir = Path(self.get_testsuite_dir()).parent / "services" / "collective"
        production_files = [
            source_dir / "collectiveTypes.h",
            source_dir / "collectiveServiceData.h",
            source_dir / "collectiveServiceData.cc",
            source_dir / "collectiveEndpoint.h",
            source_dir / "staticCollectiveEndpoint.h",
            source_dir / "staticCollectiveEndpoint.cc",
        ]
        forbidden = re.compile(r"\b(?:merlin|mercury|ember|firefly|hermes|iris|mask-mpi|mpi_[a-z0-9_]*)\b")
        violations = []
        for path in production_files:
            text = path.read_text(encoding="utf-8").lower()
            for match in forbidden.finditer(text):
                violations.append(f"{path.name}: {match.group(0)}")
        self.assertEqual([], violations, "Native-stack dependency leaked into neutral collective contract")

    def test_collective_merlin_static_processor_contract(self):
        test_dir = self.get_testsuite_dir()
        out_dir = self.get_test_output_run_dir()
        output = f"{out_dir}/merlin_static_processor_contract.out"
        error = f"{out_dir}/merlin_static_processor_contract.err"

        self.run_sst(f"{test_dir}/merlin_static_processor_contract.py", output, error)
        output_path = Path(output)
        output_path.write_text(
            "".join(
                line for line in output_path.read_text(encoding="utf-8").splitlines(keepends=True)
                if not line.startswith("WARNING: Building component")
            ),
            encoding="utf-8",
        )
        self.assertFalse(os_test_file(error, "-s"), f"Nonempty error file: {error}")
        reference = f"{test_dir}/refFiles/merlin_static_processor_contract.out"
        self.assertTrue(testing_compare_sorted_diff(
            "merlin_static_processor_contract", output, reference))

    def test_collective_merlin_static_plan_contract(self):
        test_dir = self.get_testsuite_dir()
        out_dir = self.get_test_output_run_dir()
        output = f"{out_dir}/merlin_static_plan_contract.out"
        error = f"{out_dir}/merlin_static_plan_contract.err"

        self.run_sst(f"{test_dir}/merlin_static_plan_contract.py", output, error)
        self.assertFalse(os_test_file(error, "-s"), f"Nonempty error file: {error}")
        self.assertIn("StaticCollectivePlan contract PASS",
                      Path(output).read_text(encoding="utf-8"))

    def test_collective_merlin_static_rejects_invalid_model_plan(self):
        test_dir = self.get_testsuite_dir()
        out_dir = self.get_test_output_run_dir()
        model = f"{test_dir}/merlin_static_invalid_plan.py"
        cases = {
            "swapped-port": "static collective plan does not match built fat-tree",
            "overflow": "root_router must be a nonnegative integer no greater than",
        }
        for mode, diagnostic in cases.items():
            output = f"{out_dir}/merlin_static_invalid_plan_{mode}.out"
            error = f"{out_dir}/merlin_static_invalid_plan_{mode}.err"
            self.run_sst(model, output, error, other_args=f'--model-options="{mode}"',
                         expected_rc=1, timeout_sec=5)
            combined = (Path(output).read_text(encoding="utf-8") +
                        Path(error).read_text(encoding="utf-8"))
            self.assertIn(diagnostic, combined)

    def test_collective_merlin_static_rejects_invalid_transport(self):
        test_dir = self.get_testsuite_dir()
        out_dir = self.get_test_output_run_dir()
        model = f"{test_dir}/merlin_static_ordinary_baseline.py"
        cases = {
            "bad-flit": "invalid or unsupported static local projection",
            "bad-capacity": "invalid or unsupported static local projection",
            "bad-downstream-capacity": "unsupported by initialized downstream credits",
            "disconnected": "invalid or unsupported static local projection",
        }
        for mode, diagnostic in cases.items():
            output = f"{out_dir}/merlin_static_{mode}.out"
            error = f"{out_dir}/merlin_static_{mode}.err"
            self.run_sst(model, output, error, other_args=f'--model-options="{mode}"',
                         expected_rc=1, timeout_sec=5)
            combined = (Path(output).read_text(encoding="utf-8") +
                        Path(error).read_text(encoding="utf-8"))
            self.assertIn(diagnostic, combined)

    def test_collective_merlin_static_service_disabled_baseline(self):
        test_dir = self.get_testsuite_dir()
        out_dir = self.get_test_output_run_dir()
        model = f"{test_dir}/merlin_static_ordinary_baseline.py"
        disabled_output = f"{out_dir}/merlin_static_ordinary_disabled.out"
        disabled_error = f"{out_dir}/merlin_static_ordinary_disabled.err"
        enabled_output = f"{out_dir}/merlin_static_ordinary_enabled.out"
        enabled_error = f"{out_dir}/merlin_static_ordinary_enabled.err"

        self.run_sst(model, disabled_output, disabled_error)
        self.run_sst(model, enabled_output, enabled_error, other_args='--model-options="service"')
        self.assertFalse(os_test_file(disabled_error, "-s"), "disabled baseline produced stderr")
        self.assertFalse(os_test_file(enabled_error, "-s"), "dormant-service baseline produced stderr")
        self.assertEqual(Path(disabled_output).read_bytes(), Path(enabled_output).read_bytes(),
            "installing a dormant collective processor changed ordinary traffic output or timing")
        stalls = [int(value) for value in re.findall(
            r"Nic [0-3] had ([0-9]+) stalled cycles", Path(disabled_output).read_text(encoding="utf-8"))]
        self.assertEqual(4, len(stalls), "ordinary baseline did not report every NIC")
        self.assertTrue(all(value > 0 for value in stalls),
            "ordinary baseline did not force credit backpressure on every NIC")
