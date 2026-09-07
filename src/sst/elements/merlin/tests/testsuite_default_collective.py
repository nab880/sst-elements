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
