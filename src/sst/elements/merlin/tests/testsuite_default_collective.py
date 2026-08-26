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
            source_dir / "collectiveArithmetic.h",
            source_dir / "collectiveArithmetic.cc",
            source_dir / "collectiveEndpoint.h",
            source_dir / "collectiveRoute.h",
            source_dir / "collectiveRoute.cc",
        ]
        forbidden = re.compile(r"\b(?:merlin|mercury|ember|firefly|hermes|iris|mask-mpi|mpi_[a-z0-9_]*)\b")
        violations = []
        for path in production_files:
            text = path.read_text(encoding="utf-8").lower()
            for match in forbidden.finditer(text):
                violations.append(f"{path.name}: {match.group(0)}")
        self.assertEqual([], violations, "Native-stack dependency leaked into neutral collective contract")
