# -*- coding: utf-8 -*-

import re
from pathlib import Path

from sst_unittest import *
from sst_unittest_support import *


class testcase_ember_innetwork(SSTTestCase):

    def test_ember_allreduce_disabled_baseline(self):
        test_path = self.get_testsuite_dir()
        outdir = self.get_test_output_run_dir()
        outfile = f"{outdir}/ember_allreduce_disabled.out"
        errfile = f"{outdir}/ember_allreduce_disabled.err"

        self.run_sst(
            f"{test_path}/ember_allreduce_disabled.py",
            outfile,
            errfile,
            set_cwd=test_path,
        )
        self.assertFalse(
            os_test_file(errfile, "-s"),
            f"Disabled Firefly allreduce produced stderr: {errfile}",
        )

        text = Path(outfile).read_text(encoding="utf-8")
        self.assertEqual(
            1,
            text.count("Simulation is complete, simulated time: 10.426 us"),
        )
        self.assertEqual(
            1,
            text.count(
                "Allreduce: ranks 4, loop 2, 4 double(s), latency 2.709 us"
            ),
        )
        verification = [
            line
            for line in text.splitlines()
            if line.startswith("Ember Allreduce verify rank ")
        ]
        self.assertEqual(4, len(verification))
        self.assertEqual(
            {
                f"Ember Allreduce verify rank {rank} input {rank + 1:.6f} "
                "result 10.000000 PASS"
                for rank in range(4)
            },
            set(verification),
        )
        self.assertNotIn("Firefly Allreduce rank ", text)

        statistic = re.compile(
            r"^\s+(?P<component>nic[0-3])\.(?P<name>sentPkts|rcvdPkts) : "
            r"Accumulator : Sum\.u64 = (?P<sum>[0-9]+);",
            re.MULTILINE,
        )
        statistic_matches = list(statistic.finditer(text))
        self.assertEqual(8, len(statistic_matches))
        statistics = {
            (match.group("component"), match.group("name")): int(match.group("sum"))
            for match in statistic_matches
        }
        self.assertEqual(
            {
                (f"nic{nic}", name): 6 if nic < 2 else 3
                for nic in range(4)
                for name in ("sentPkts", "rcvdPkts")
            },
            statistics,
        )
        for name in (
                "collectiveEnqueued",
                "collectiveSchedulerSends", "collectiveSendRetries",
                "collectiveResultsCompleted"):
            self.assertNotIn(f".{name} :", text)

    def test_ember_allreduce_innetwork_supported(self):
        self._allreduce_innetwork("supported")

    def test_ember_allreduce_innetwork_fallback(self):
        self._allreduce_innetwork("fallback")

    def test_ember_allreduce_innetwork_unsupported(self):
        self._allreduce_innetwork("unsupported")

    def _allreduce_innetwork(self, mode):
        test_path = self.get_testsuite_dir()
        outdir = self.get_test_output_run_dir()
        tmpdir = self.get_test_output_tmp_dir()
        outfile = f"{outdir}/ember_allreduce_innetwork_{mode}.out"
        errfile = f"{outdir}/ember_allreduce_innetwork_{mode}.err"
        cmpfile = f"{tmpdir}/ember_allreduce_innetwork_{mode}.cmp"

        self.run_sst(
            f"{test_path}/ember_allreduce_innetwork.py",
            outfile,
            errfile,
            set_cwd=test_path,
            other_args=f'--model-options="{mode}"',
        )
        self.assertFalse(
            os_test_file(errfile, "-s"),
            f"Ember in-network allreduce produced stderr: {errfile}",
        )

        router_names = {
            "rtr_l0_g0_r0": 0,
            "rtr_l0_g1_r0": 1,
            "rtr_l1_g0_r0": 2,
        }
        statistic = re.compile(
            r"^\s+(?P<component>\S*rtr_l[01]_g[01]_r0)\."
            r"(?P<name>[a-z_]+) : Accumulator : Sum\.u64 = (?P<sum>[0-9]+);"
        )
        nic_statistic = re.compile(
            r"^\s+(?P<component>nic[0-3])\."
            r"(?P<name>collective[A-Za-z]+) : Accumulator : "
            r"Sum\.u64 = (?P<sum>[0-9]+);"
        )
        statistics = {}
        nic_statistics = {}
        semantic_lines = []
        output_lines = Path(outfile).read_text(encoding="utf-8").splitlines(keepends=True)
        self.assertEqual(
            1,
            sum(line.startswith("Simulation is complete") for line in output_lines),
            "Ember in-network allreduce did not complete exactly once",
        )
        for line in output_lines:
            match = statistic.match(line)
            if match:
                component = match.group("component").split(".")[-1]
                key = (router_names[component], match.group("name"))
                self.assertNotIn(key, statistics, f"Duplicate statistic {key}")
                statistics[key] = int(match.group("sum"))
                continue

            nic_match = nic_statistic.match(line)
            if nic_match:
                key = (nic_match.group("component"), nic_match.group("name"))
                self.assertNotIn(key, nic_statistics, f"Duplicate NIC statistic {key}")
                nic_statistics[key] = int(nic_match.group("sum"))
            elif line.startswith("Firefly Allreduce rank ") or line.startswith(
                    "Ember Allreduce verify rank "):
                semantic_lines.append(line)

        self.assertEqual(8, len(semantic_lines),
            "Expected one path report and one verified result per rank")
        Path(cmpfile).write_text("".join(semantic_lines), encoding="utf-8")
        reference_mode = "supported" if mode == "supported" else "fallback"
        reference = f"{test_path}/refFiles/ember_allreduce_innetwork_{reference_mode}.out"
        self.assertTrue(testing_compare_sorted_diff(
            f"ember_allreduce_innetwork_{mode}", cmpfile, reference))

        processor_names = (
            "local_contributions",
            "child_contributions",
            "parent_results",
            "upward_aggregates",
            "result_packets",
            "active_high_water",
            "installed_branch_slots",
        )
        names = processor_names + (
            "network_service_accept",
            "network_service_synthetic",
        )
        expected_keys = {
            (router, name)
            for router in range(3)
            for name in names + ("egress_retries",)
        }
        self.assertEqual(expected_keys, set(statistics),
            "Ember allreduce emitted a missing or unexpected statistic")

        if mode == "supported":
            expected = {
                0: (2, 0, 1, 1, 2, 1, 2, 3, 3),
                1: (2, 0, 1, 1, 2, 1, 2, 3, 3),
                2: (0, 2, 0, 0, 2, 1, 2, 2, 2),
            }
        else:
            expected = {
                router: (0, 0, 0, 0, 0, 0, 2, 0, 0)
                for router in range(3)
            }

        for router, values in expected.items():
            for name, value in zip(names, values):
                self.assertEqual(value, statistics[(router, name)],
                    f"Wrong {mode} counter for router {router} {name}")

        tree_packets = sum(
            statistics[(router, name)]
            for router in range(3)
            for name in ("local_contributions", "upward_aggregates", "result_packets")
        )
        self.assertEqual(12 if mode == "supported" else 0, tree_packets,
            "Static-tree traffic must equal 2E, or zero on software fallback")
        self.assertEqual(8 if mode == "supported" else 0,
            sum(statistics[(router, "network_service_accept")] for router in range(3)))
        self.assertEqual(8 if mode == "supported" else 0,
            sum(statistics[(router, "network_service_synthetic")] for router in range(3)))

        if mode != "supported":
            for router in range(3):
                self.assertEqual(0, statistics[(router, "egress_retries")],
                    f"Fallback exercised router {router} service egress")

        nic_names = (
            "collectiveEnqueued",
            "collectiveSchedulerSends",
            "collectiveSendRetries",
            "collectiveResultsCompleted",
        )
        expected_nic_keys = {
            (f"nic{nic}", name) for nic in range(4) for name in nic_names
        }
        self.assertEqual(expected_nic_keys, set(nic_statistics),
            "Ember allreduce emitted a missing or unexpected NIC statistic")
        for nic in range(4):
            expected_value = 1 if mode == "supported" else 0
            for name in (
                    "collectiveEnqueued",
                    "collectiveSchedulerSends",
                    "collectiveResultsCompleted",
            ):
                self.assertEqual(expected_value, nic_statistics[(f"nic{nic}", name)],
                    f"Wrong {mode} NIC counter for nic{nic} {name}")
            self.assertGreaterEqual(nic_statistics[(f"nic{nic}", "collectiveSendRetries")], 0)
