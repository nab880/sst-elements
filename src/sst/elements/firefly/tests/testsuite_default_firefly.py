# -*- coding: utf-8 -*-

import sys
import shlex
from pathlib import Path
import re

from sst_unittest import *
from sst_unittest_support import *

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "merlin" / "tests"))
from collective_test_stats import read_statistics


class testcase_firefly(SSTTestCase):

    def test_firefly_collective_host_timing(self):
        test_dir = Path(self.get_testsuite_dir())
        out_dir = Path(self.get_test_output_run_dir())
        timings = {}
        for case in ("baseline", "read", "write_baseline", "write", "submit", "completion"):
            output = out_dir / f"firefly_collective_host_{case}.out"
            error = out_dir / f"firefly_collective_host_{case}.err"
            statfile = out_dir / f"firefly_collective_host_{case}.csv"
            model_options = shlex.join((case, str(statfile)))
            self.run_sst(str(test_dir / "collective_host_timing.py"), str(output), str(error),
                other_args="--model-options=" + shlex.quote(model_options), timeout_sec=10)
            self.assertFalse(os_test_file(str(error), "-s"), f"Nonempty error file: {error}")
            text = output.read_text(encoding="utf-8")
            rows = re.findall(r"host timing rank=(\d) invocation=(\d) elapsed_ns=(\d+) in_place=PASS", text)
            self.assertEqual(4, len(rows), text)
            timings[case] = {(int(rank), int(invocation)): int(elapsed) for rank, invocation, elapsed in rows}
            statistics = {key: int(row["Sum.u64"]) for key, row in
                          read_statistics(statfile, testing_check_get_num_ranks()).items()}
            self.assertEqual({(f"nic{rank}", name): value for rank in range(2)
                              for name, value in (("collectiveDmaReadBytes", 16),
                                                  ("collectiveDmaWriteBytes", 16),
                                                  ("collectiveResultsCompleted", 2))}, statistics)
        # Reads and both explicit overheads gate each invocation's completion.
        for case in ("read", "submit", "completion"):
            for key, baseline in timings["baseline"].items():
                self.assertAlmostEqual(100, timings[case][key] - baseline, delta=2,
                    msg=f"{case}: rank/invocation {key} bypassed the configured host cost")
        # SimpleMemoryModel posts writes. Their latency holds its only memory
        # slot, delaying the next invocation's read rather than the first
        # write's DMA callback. This proves writes consume the shared resource.
        for key, baseline in timings["write_baseline"].items():
            self.assertAlmostEqual(0 if key[1] == 1 else 100, timings["write"][key] - baseline,
                delta=2, msg=f"posted write did not retain memory occupancy for {key}")

    def test_firefly_collective_host_invalid_delay(self):
        test_dir = Path(self.get_testsuite_dir())
        out_dir = Path(self.get_test_output_run_dir())
        for case in ("negative_submit", "negative_completion", "overflow_submit", "overflow_completion"):
            output = out_dir / f"firefly_collective_host_{case}.out"
            error = out_dir / f"firefly_collective_host_{case}.err"
            self.run_sst(str(test_dir / "collective_host_timing.py"), str(output), str(error),
                other_args=f'--model-options="{case}"', expected_rc=1, timeout_sec=10)
            text = output.read_text(encoding="utf-8") + error.read_text(encoding="utf-8")
            self.assertIn("delays must be nonnegative and fit the timebase", text)

    def test_firefly_empty_request_disabled_baseline(self):
        test_dir = self.get_testsuite_dir()
        out_dir = self.get_test_output_run_dir()
        output = f"{out_dir}/firefly_empty_request_regression.out"
        error = f"{out_dir}/firefly_empty_request_regression.err"
        statfile = Path(out_dir) / "firefly_empty_request_regression.csv"
        self.run_sst(f"{test_dir}/empty_request_regression.py",
            output, error, timeout_sec=10,
            other_args="--model-options=" + shlex.quote(shlex.quote(str(statfile))))
        self.assertFalse(os_test_file(error, "-s"), f"Nonempty error file: {error}")
        text = Path(output).read_text(encoding="utf-8")
        self.assertEqual(1, text.count("Firefly ordinary empty Request:"))
        statistics = {key: int(row["Sum.u64"]) for key, row in
                      read_statistics(statfile, testing_check_get_num_ranks()).items()}
        self.assertEqual({("nic", "rcvdPkts"): 1}, statistics)
        for statistic in (
                "collectiveEnqueued",
                "collectiveSchedulerSends", "collectiveSendRetries",
                "collectiveResultsCompleted", "collectiveDmaReadBytes", "collectiveDmaWriteBytes"):
            self.assertNotIn(("nic", statistic), statistics)

    def test_firefly_collective_mapped_repeat(self):
        test_dir = Path(self.get_testsuite_dir())
        out_dir = Path(self.get_test_output_run_dir())
        source = (test_dir.parent.parent / "ember" / "tests" /
            "ember_allreduce_innetwork.py").read_text(encoding="utf-8")
        # The plan follows the job's allocation, so a random placement needs
        # no hand-written participant map.
        self.assertIn("Allreduce iterations=1", source)
        self.assertIn('system.allocateNodes(job, "linear")', source)
        source = source.replace("Allreduce iterations=1", "Allreduce iterations=2")
        source = source.replace(
            'system.allocateNodes(job, "linear")',
            'system.allocateNodes(job, "random", 2)')

        config = out_dir / "firefly_collective_mapped_repeat.py"
        output = out_dir / "firefly_collective_mapped_repeat.out"
        error = out_dir / "firefly_collective_mapped_repeat.err"
        config.write_text(source, encoding="utf-8")
        self.run_sst(str(config), str(output), str(error),
            other_args='--model-options="supported"', timeout_sec=10)
        self.assertFalse(os_test_file(str(error), "-s"), f"Nonempty error file: {error}")
        text = output.read_text(encoding="utf-8")
        self.assertNotIn("SOFTWARE FALLBACK", text)
        self.assertNotIn("Event queue empty", text)
        self.assertEqual(1, text.count("Simulation is complete"))
        for rank in range(4):
            self.assertEqual(2, text.count(
                f"Firefly Allreduce rank {rank} OFFLOAD ACCEPTED"))
            self.assertEqual(1, text.count(f"Ember Allreduce verify rank {rank} "))
        self.assertIn("Allreduce: ranks 4, loop 2", text)

    def test_firefly_collective_required_service_missing(self):
        test_dir = Path(self.get_testsuite_dir())
        out_dir = Path(self.get_test_output_run_dir())
        source = (test_dir.parent.parent / "ember" / "tests" /
            "ember_allreduce_innetwork.py").read_text(encoding="utf-8")
        self.assertIn("networkif.network_service_id = SERVICE_ID", source)
        source = source.replace(
            "networkif.network_service_id = SERVICE_ID",
            "networkif.network_service_id = 0")
        config = out_dir / "firefly_collective_missing_service.py"
        output = out_dir / "firefly_collective_missing_service.out"
        error = out_dir / "firefly_collective_missing_service.err"
        config.write_text(source, encoding="utf-8")
        self.run_sst(str(config), str(output), str(error),
            other_args='--model-options="supported"', expected_rc=1, timeout_sec=10)
        combined = output.read_text(encoding="utf-8") + error.read_text(encoding="utf-8")
        self.assertIn(
            "collectiveEnable=true but no validated collective service route is available",
            combined)
        self.assertNotIn("Simulation is complete", combined)
