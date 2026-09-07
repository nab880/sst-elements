# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.

import sys
import shlex
from pathlib import Path

from sst_unittest import SSTTestCase
from sst_unittest_support import testing_check_get_num_ranks

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "merlin" / "tests"))
from collective_test_stats import read_statistics


class testcase_ember_sparse_collective(SSTTestCase):
    def test_sparse_collective_preserves_physical_ports(self):
        output, error, statistics = self._run("supported")
        self.assertEqual("", error)
        self.assertEqual(1, output.count("Simulation is complete"))
        self.assertNotIn("SOFTWARE FALLBACK", output)
        self.assertNotIn("Event queue empty", output)
        for rank in range(2):
            self.assertEqual(2, output.count(f"Firefly Allreduce rank {rank} OFFLOAD ACCEPTED"))
            self.assertEqual(1, output.count(
                f"Ember Allreduce verify rank {rank} input {rank + 1:.6f} result 3.000000 PASS"))
        self.assertEqual(2, output.count("Ember Allreduce verify rank "))
        contributions = {component: int(row["Sum.u64"]) for (component, name), row in statistics.items()
                         if name == "local_contributions"}
        self.assertEqual({"rtr_l0_g0_r0": 2, "rtr_l0_g1_r0": 2, "rtr_l1_g0_r0": 0},
                         contributions)

    def test_sparse_collective_rejects_mismatched_job(self):
        cases = {
            "physical-mismatch": "physical membership does not match",
            "logical-mismatch": "logical membership does not match",
            "missing-rank": "every logical rank exactly once",
            "reallocated": "physical membership does not match",
            "reordered": "logical membership does not match",
        }
        for mode, diagnostic in cases.items():
            with self.subTest(mode=mode):
                output, error, _ = self._run(mode, expected_rc=1)
                self.assertIn(diagnostic, output + error)
                self.assertNotIn("OFFLOAD ACCEPTED", output)
                self.assertNotIn("Simulation is complete", output)

    def _run(self, mode, expected_rc=0):
        test_dir = Path(self.get_testsuite_dir())
        out_dir = Path(self.get_test_output_run_dir())
        output = out_dir / f"ember_sparse_collective_{mode}.out"
        error = out_dir / f"ember_sparse_collective_{mode}.err"
        statfile = out_dir / f"ember_sparse_collective_{mode}.csv"
        model_options = shlex.join((mode, str(statfile)))
        self.run_sst(str(test_dir / "ember_allreduce_sparse.py"), str(output), str(error),
                     other_args="--model-options=" + shlex.quote(model_options),
                     expected_rc=expected_rc, timeout_sec=10)
        statistics = read_statistics(statfile, testing_check_get_num_ranks()) if expected_rc == 0 else {}
        return output.read_text(encoding="utf-8"), error.read_text(encoding="utf-8"), statistics
