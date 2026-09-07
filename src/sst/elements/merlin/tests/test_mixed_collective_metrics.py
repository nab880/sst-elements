#!/usr/bin/env python3
# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
"""Guard against declaring progress from aggregate or post-collective traffic."""

import csv
from pathlib import Path
import tempfile
import unittest

from run_mixed_collective import DEFAULTS, ENDPOINTS, check_baselines, extract_metrics


class MixedCollectiveMetrics(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.case = dict(DEFAULTS, name="test", iterations=2, duration_us=10, sample_us=2)
        output = "\n".join("Firefly Allreduce rank {} OFFLOAD ACCEPTED".format(rank)
                           for _ in range(2) for rank in (0, 1))
        output += "\n" + "\n".join("Ember Allreduce verify rank {} result 3 PASS".format(rank)
                                   for rank in (0, 1))
        output += "\nAllreduce: ranks 2, loop 2, 1 double(s), latency 3 us\n"
        output += "Simulation is complete, simulated time: 10 us\n"
        (self.directory / "stdout.txt").write_text(output)
        (self.directory / "motifs.log").write_text(
            "0 0 1 Allreduce 2 us 8 us\n0 1 1 Allreduce 2 us 8 us\n")

    def write_statistics(self, stalled=None, only_after=False):
        fields = ["ComponentName", "StatisticName", "StatisticSubId", "StatisticType", "SimTime",
                  "Sum.u64", "SumSQ.u64", "Count.u64", "Min.u64", "Max.u64"]
        with (self.directory / "statistics.csv").open("w", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(fields)
            for endpoint in ENDPOINTS:
                count = 0
                for time in (2, 4, 6, 8, 10):
                    if not (endpoint == stalled and time == 6) and not (only_after and time < 10):
                        count += 100
                    writer.writerow(["offered_load_{}:networkIF".format(endpoint), "packet_latency", "",
                                     "Accumulator", time * 1000000, count * 10, count * 100,
                                     count, 10, 10])
            for name in ("network_service_accept", "network_service_synthetic"):
                writer.writerow(["router", name, "", "Accumulator", 10000000, 12, 12, 12, 1, 1])

    def test_progress_uses_only_full_overlap_bins(self):
        self.write_statistics()
        result = extract_metrics(self.case, self.directory)
        self.assertEqual(result["ordinary_overlap_packets"], 1800)
        self.assertEqual(result["minimum_endpoint_overlap_sample_packets"], 100)
        self.assertEqual(result["ordinary_endpoints"]["1"]["overlap_samples"][0]["start_ns"], 2000)

    def test_one_stalled_endpoint_cannot_hide_in_aggregate(self):
        self.write_statistics(stalled=3)
        with self.assertRaisesRegex(ValueError, "endpoint 3 made no progress"):
            extract_metrics(self.case, self.directory)

    def test_traffic_after_collective_cannot_establish_overlap(self):
        self.write_statistics(only_after=True)
        with self.assertRaisesRegex(ValueError, "made no progress"):
            extract_metrics(self.case, self.directory)

    def test_duplicate_completion_does_not_count_as_success(self):
        self.write_statistics()
        with (self.directory / "stdout.txt").open("a") as stream:
            stream.write("Ember Allreduce verify rank 0 result 3 PASS\n")
        with self.assertRaisesRegex(ValueError, "both ranks must verify"):
            extract_metrics(self.case, self.directory)

    def test_baselines_require_exact_statistics(self):
        baseline = dict(mode="disabled", name="disabled", arbiter="lru", load=0.5,
                        ordinary_statistics_sha256="one")
        dormant = dict(baseline, mode="dormant", name="dormant", ordinary_statistics_sha256="two")
        with self.assertRaisesRegex(ValueError, "statistics differ"):
            check_baselines([baseline, dormant])


if __name__ == "__main__":
    unittest.main()
