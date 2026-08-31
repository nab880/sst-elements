# -*- coding: utf-8 -*-

from pathlib import Path

from sst_unittest import *
from sst_unittest_support import *


class testcase_ember_innetwork(SSTTestCase):

    def test_ember_allreduce_modes(self):
        test_dir = Path(self.get_testsuite_dir())
        out_dir = Path(self.get_test_output_run_dir())
        verification = [
            f"Ember Allreduce verify rank {rank} input {rank + 1:.6f} "
            "result 10.000000 PASS" for rank in range(4)]
        for mode in ("disabled", "supported", "fallback", "unsupported", "mapped", "missing"):
            with self.subTest(mode=mode):
                output = out_dir / f"ember_allreduce_{mode}.out"
                error = out_dir / f"ember_allreduce_{mode}.err"
                self.run_sst(str(test_dir / "ember_allreduce_innetwork.py"),
                    str(output), str(error), set_cwd=str(test_dir),
                    other_args=f'--model-options="{mode}"',
                    expected_rc=1 if mode == "missing" else 0, timeout_sec=10)
                text = output.read_text(encoding="utf-8") + error.read_text(encoding="utf-8")

                if mode == "missing":
                    self.assertIn("collectiveEnable=true but no validated collective service route is available", text)
                    self.assertNotIn("Simulation is complete", text)
                    continue

                self.assertFalse(os_test_file(str(error), "-s"), f"Nonempty error file: {error}")
                self.assertCountEqual(verification, [
                    line for line in text.splitlines()
                    if line.startswith("Ember Allreduce verify rank ")])
                self.assertEqual(1, text.count("Simulation is complete"))
                self.assertNotIn("Event queue empty", text)

                if mode == "disabled":
                    self.assertIn("Simulation is complete, simulated time: 10.426 us", text)
                    self.assertIn("Allreduce: ranks 4, loop 2, 4 double(s), latency 2.709 us", text)
                    self.assertNotIn("Firefly Allreduce rank ", text)
                    for nic in range(4):
                        packets = 6 if nic < 2 else 3
                        for name in ("sentPkts", "rcvdPkts"):
                            self.assertEqual(1, text.count(
                                f"nic{nic}.{name} : Accumulator : Sum.u64 = {packets};"))
                else:
                    report = "OFFLOAD ACCEPTED" if mode in ("supported", "mapped") else "SOFTWARE FALLBACK"
                    opposite = "SOFTWARE FALLBACK" if report == "OFFLOAD ACCEPTED" else "OFFLOAD ACCEPTED"
                    self.assertNotIn(opposite, text)
                    reports_per_rank = 2 if mode == "mapped" else 1
                    for rank in range(4):
                        self.assertEqual(reports_per_rank, text.count(
                            f"Firefly Allreduce rank {rank} {report}"))
                    if mode == "mapped":
                        self.assertIn("Allreduce: ranks 4, loop 2", text)
