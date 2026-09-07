# -*- coding: utf-8 -*-

import re
import shlex
import sys
from pathlib import Path

from sst_unittest import *
from sst_unittest_support import *

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "merlin" / "tests"))
from collective_test_stats import read_statistics


class testcase_ember_innetwork(SSTTestCase):
    # The offload folds contributions in branch order with a rounding point
    # per addition, while the software tree folds in its own order.  The
    # supported and fallback runs are compared against one reference, which
    # only holds for exactly representable inputs such as the small integers
    # used here.  Widen the inputs and the comparison needs a tolerance.


    def test_ember_allreduce_disabled_baseline(self):
        test_path = self.get_testsuite_dir()
        outdir = self.get_test_output_run_dir()
        outfile = f"{outdir}/ember_allreduce_disabled.out"
        errfile = f"{outdir}/ember_allreduce_disabled.err"
        statfile = Path(outdir) / "ember_allreduce_disabled.csv"

        self.run_sst(
            f"{test_path}/ember_allreduce_disabled.py",
            outfile,
            errfile,
            set_cwd=test_path,
            other_args="--model-options=" + shlex.quote(shlex.quote(str(statfile))),
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

        statistics = {key: int(row["Sum.u64"]) for key, row in
                      read_statistics(statfile, testing_check_get_num_ranks()).items()}
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
            self.assertFalse(any(statistic == name for _, statistic in statistics))

    def test_ember_allreduce_innetwork_supported(self):
        self._allreduce_innetwork("supported")

    def test_ember_allreduce_innetwork_fallback(self):
        self._allreduce_innetwork("fallback")

    def test_ember_allreduce_innetwork_unsupported(self):
        self._allreduce_innetwork("unsupported")

    def test_ember_allreduce_innetwork_identity_vns(self):
        self._allreduce_innetwork("supported", "identity-vns")

    def test_ember_allreduce_innetwork_multivc_credits(self):
        test_path = self.get_testsuite_dir()
        outdir = self.get_test_output_run_dir()
        for mode in ("host", "host-swapped", "routed", "routed-swapped"):
            with self.subTest(mode=mode):
                outfile = f"{outdir}/ember_allreduce_multivc_{mode}.out"
                errfile = f"{outdir}/ember_allreduce_multivc_{mode}.err"
                statfile = Path(outdir) / f"ember_allreduce_multivc_{mode}.csv"
                model_options = "{} {}".format(mode, shlex.quote(str(statfile)))
                self.run_sst(f"{test_path}/ember_allreduce_multivc.py", outfile, errfile,
                             other_args="--model-options=" + shlex.quote(model_options),
                             timeout_sec=30)
                self.assertFalse(os_test_file(errfile, "-s"))
                text = Path(outfile).read_text(encoding="utf-8")
                self.assertEqual(1, text.count("Simulation is complete"))
                self.assertEqual(8, text.count("OFFLOAD ACCEPTED"))
                verification = [line for line in text.splitlines()
                                if line.startswith("Ember Allreduce verify rank ")]
                self.assertEqual([
                    f"Ember Allreduce verify rank {rank} input {rank + 1:.6f} result 3.000000 PASS"
                    for rank in range(2)
                ], sorted(verification))
                self.assertIn("Allreduce: ranks 2, loop 4, 1 double(s)", text)
                names = ("network_service_accept", "network_service_synthetic")
                statistics = {key: int(row["Sum.u64"]) for key, row in
                              read_statistics(statfile, testing_check_get_num_ranks()).items()
                              if key[1] in names}
                components = ("rtr_0", "rtr_1") if mode.startswith("routed") else ("rtr_0",)
                self.assertEqual({(component, name) for component in components for name in names},
                                 set(statistics))
                for statistic in names:
                    packets = sum(statistics[(component, statistic)] for component in components)
                    self.assertEqual(16 if mode.startswith("routed") else 8, packets)

    def test_ember_allreduce_innetwork_rejects_remapped_vns(self):
        test_path = self.get_testsuite_dir()
        outdir = self.get_test_output_run_dir()
        outfile = f"{outdir}/ember_allreduce_remapped_vns.out"
        errfile = f"{outdir}/ember_allreduce_remapped_vns.err"
        self.run_sst(f"{test_path}/ember_allreduce_innetwork.py", outfile, errfile,
                     other_args='--model-options="supported remapped-vns"',
                     expected_rc=1, timeout_sec=5)
        combined = Path(outfile).read_text() + Path(errfile).read_text()
        self.assertIn("collectiveEnable=true but no validated collective service route is available", combined)
        self.assertNotIn("OFFLOAD ACCEPTED", combined)

    def test_ember_allreduce_router_resources(self):
        runs = {profile: self._allreduce_innetwork("supported", profile)
                for profile in ("default", "flit4", "flit16", "fast-ingress", "wide-ingress",
                                "private-ingress", "slow-reduction")}
        def elapsed_ns(output):
            match = re.search(r"Simulation is complete, simulated time: ([0-9.]+) (ps|ns|us|ms|s)", output)
            self.assertIsNotNone(match, "missing completion time")
            return float(match.group(1)) * {"ps": .001, "ns": 1, "us": 1000, "ms": 1e6, "s": 1e9}[match.group(2)]
        baseline = elapsed_ns(runs["default"])
        self.assertGreater(elapsed_ns(runs["slow-reduction"]), baseline,
                           "configured reducer latency was not charged")
        self.assertLess(elapsed_ns(runs["fast-ingress"]), baseline,
                        "service transfer bandwidth did not affect latency")
        self.assertLessEqual(elapsed_ns(runs["wide-ingress"]), baseline)

    def test_ember_allreduce_rejects_invalid_ingress_resources(self):
        test_path = self.get_testsuite_dir()
        outdir = self.get_test_output_run_dir()
        for profile in ("zero-width", "zero-bandwidth"):
            outfile, errfile = f"{outdir}/{profile}.out", f"{outdir}/{profile}.err"
            self.run_sst(f"{test_path}/ember_allreduce_innetwork.py", outfile, errfile,
                         other_args=f'--model-options="supported {profile}"', expected_rc=1, timeout_sec=5)
            self.assertIn("ingress width and bandwidth must be positive",
                          Path(outfile).read_text() + Path(errfile).read_text())

    def _allreduce_innetwork(self, mode, profile="default"):
        run_name = f"{mode}_{profile}"
        test_path = self.get_testsuite_dir()
        outdir = self.get_test_output_run_dir()
        tmpdir = self.get_test_output_tmp_dir()
        outfile = f"{outdir}/ember_allreduce_innetwork_{run_name}.out"
        errfile = f"{outdir}/ember_allreduce_innetwork_{run_name}.err"
        cmpfile = f"{tmpdir}/ember_allreduce_innetwork_{run_name}.cmp"
        statfile = Path(outdir) / f"ember_allreduce_innetwork_{run_name}.csv"
        model_options = shlex.join((mode, profile, str(statfile)))

        self.run_sst(
            f"{test_path}/ember_allreduce_innetwork.py",
            outfile,
            errfile,
            set_cwd=test_path,
            other_args="--model-options=" + shlex.quote(model_options),
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
        statistics = {}
        nic_statistics = {}
        for (component, name), row in read_statistics(statfile, testing_check_get_num_ranks()).items():
            if component in router_names:
                statistics[(router_names[component], name)] = int(row["Sum.u64"])
            else:
                nic_statistics[(component, name)] = int(row["Sum.u64"])
        semantic_lines = []
        output_lines = Path(outfile).read_text(encoding="utf-8").splitlines(keepends=True)
        self.assertEqual(
            1,
            sum(line.startswith("Simulation is complete") for line in output_lines),
            "Ember in-network allreduce did not complete exactly once",
        )
        for line in output_lines:
            if line.startswith("Firefly Allreduce rank ") or line.startswith(
                    "Ember Allreduce verify rank "):
                semantic_lines.append(line)

        self.assertEqual(8, len(semantic_lines),
            "Expected one path report and one verified result per rank")
        Path(cmpfile).write_text("".join(semantic_lines), encoding="utf-8")
        reference_mode = "supported" if mode == "supported" else "fallback"
        reference = f"{test_path}/refFiles/ember_allreduce_innetwork_{reference_mode}.out"
        self.assertTrue(testing_compare_sorted_diff(
            f"ember_allreduce_innetwork_{run_name}", cmpfile, reference))

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

        return Path(outfile).read_text(encoding="utf-8")
