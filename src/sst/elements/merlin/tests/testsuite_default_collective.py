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
        # The contract headers may include SST core, the standard library,
        # and each other; never a native stack or Merlin internals.
        source_dir = Path(self.get_testsuite_dir()).parent / "services" / "collective"
        contract_headers = (
            "collectiveTypes.h",
            "collectiveServiceData.h",
            "collectiveEndpoint.h",
            "staticCollectiveEndpoint.h",
        )
        include = re.compile(r'^\s*#include\s+([<"])([^>"]+)[>"]')
        for name in contract_headers:
            for line in (source_dir / name).read_text(encoding="utf-8").splitlines():
                match = include.match(line)
                if not match:
                    continue
                bracket, target = match.groups()
                if bracket == "<":
                    self.assertTrue(target.startswith("sst/core/") or "/" not in target,
                        f"{name} includes {target}")
                else:
                    self.assertIn(target, contract_headers, f"{name} includes {target}")

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
            "overflow": "root_router must be a nonnegative integer no greater than",
            "wrong-root": "is not a router of this fat tree",
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

        for arbiter in ("", "lru"):
            self.run_sst(model, disabled_output, disabled_error,
                         other_args=f'--model-options="disabled {arbiter}"')
            self.run_sst(model, enabled_output, enabled_error,
                         other_args=f'--model-options="service {arbiter}"')
            self.assertFalse(os_test_file(disabled_error, "-s"), "disabled baseline produced stderr")
            self.assertFalse(os_test_file(enabled_error, "-s"), "dormant-service baseline produced stderr")
            self.assertEqual(Path(disabled_output).read_bytes(), Path(enabled_output).read_bytes(),
                f"dormant processor changed ordinary traffic with arbiter {arbiter or 'default'}")
        stalls = [int(value) for value in re.findall(
            r"Nic [0-3] had ([0-9]+) stalled cycles", Path(disabled_output).read_text(encoding="utf-8"))]
        self.assertEqual(4, len(stalls), "ordinary baseline did not report every NIC")
        self.assertTrue(all(value > 0 for value in stalls),
            "ordinary baseline did not force credit backpressure on every NIC")

    @unittest.skipIf(testing_check_get_num_ranks() > 1, "checkpoint test runs serially")
    @unittest.skipIf(testing_check_get_num_threads() > 1, "checkpoint test runs serially")
    def test_collective_merlin_static_checkpoint(self):
        # Checkpoint a router with the processor installed, restart from the
        # first checkpoint, and require the restarted run to reproduce the
        # original run's remaining output.
        test_dir = self.get_testsuite_dir()
        out_dir = Path(self.get_test_output_run_dir())
        model = f"{test_dir}/merlin_static_ordinary_baseline.py"
        prefix = "merlin_static_checkpoint"
        checkpoint_dir = out_dir / f"{prefix}_dir"
        generate_output = out_dir / f"{prefix}_generate.out"
        generate_error = out_dir / f"{prefix}_generate.err"
        restart_output = out_dir / f"{prefix}_restart.out"
        restart_error = out_dir / f"{prefix}_restart.err"

        self.run_sst(model, str(generate_output), str(generate_error), other_args=(
            '--model-options="service" --checkpoint-sim-period=1us '
            f"--checkpoint-prefix={prefix} --checkpoint-name-format='%p_%n' "
            f"--output-directory={checkpoint_dir}"))
        self.assertFalse(os_test_file(str(generate_error), "-s"),
            f"checkpointing run produced stderr: {generate_error}")
        checkpoint = checkpoint_dir / prefix / f"{prefix}_1" / f"{prefix}_1.sstcpt"
        self.assertTrue(checkpoint.is_file(),
            f"no checkpoint at {checkpoint}: {sorted(checkpoint_dir.rglob('*'))}")

        self.run_sst(str(checkpoint), str(restart_output), str(restart_error),
            other_args="--load-checkpoint")
        self.assertFalse(os_test_file(str(restart_error), "-s"),
            f"restarted run produced stderr: {restart_error}")

        def simulation_lines(path):
            return [line for line in path.read_text(encoding="utf-8").splitlines()
                    if line and not line.startswith("#") and not line.startswith("WARNING")]

        generated = simulation_lines(generate_output)
        restarted = simulation_lines(restart_output)
        self.assertEqual(4, sum(line.startswith("Nic ") for line in restarted),
            "restarted run did not finish every NIC")
        self.assertEqual(1, sum(line.startswith("Simulation is complete") for line in restarted))
        self.assertTrue(set(restarted).issubset(set(generated)),
            "restarted run diverged from the checkpointing run: %s" %
            sorted(set(restarted) - set(generated)))
