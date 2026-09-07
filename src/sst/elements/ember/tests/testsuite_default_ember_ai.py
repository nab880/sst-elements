# -*- coding: utf-8 -*-

import re
from pathlib import Path

from sst_unittest import *
from sst_unittest_support import *


class testcase_ember_ai(SSTTestCase):

    def test_data_parallel_training_paths(self):
        test_dir = Path(self.get_testsuite_dir())
        model = test_dir.parent / "examples" / "ai" / "data_parallel_training.py"
        out_dir = Path(self.get_test_output_run_dir())
        epochs = 2

        for scenario in ("scalar", "hybrid"):
            for path in ("software", "offload"):
                with self.subTest(scenario=scenario, path=path):
                    output = out_dir / f"ember_ai_{scenario}_{path}.out"
                    error = out_dir / f"ember_ai_{scenario}_{path}.err"
                    self.run_sst(
                        str(model),
                        str(output),
                        str(error),
                        set_cwd=str(model.parent),
                        other_args=(
                            f'--model-options="{scenario} {path} {epochs} 0 report"'
                        ),
                        timeout_sec=20,
                    )
                    self._check_output(output, scenario, path, epochs)

    def _check_output(self, output, scenario, path, epochs):
        text = output.read_text(encoding="utf-8")
        parameters = 1 if scenario == "scalar" else 4
        collectives_per_epoch = 1 if scenario == "scalar" else 2
        accepted_per_epoch = 1 if path == "offload" else 0

        self.assertEqual(1, text.count("Simulation is complete"))
        summaries = [
            line for line in text.splitlines()
            if line.startswith("AITraining rank ")
        ]
        self.assertEqual(8, len(summaries))
        for rank in range(8):
            self.assertEqual(1, sum(
                line.startswith(f"AITraining rank {rank}: ranks 8, epochs {epochs}, ")
                and f"parameters {parameters}, " in line
                and f"loss-sync {'off' if scenario == 'scalar' else 'on'}," in line
                for line in summaries
            ))
        verification = [
            line
            for line in text.splitlines()
            if line.startswith("Ember AI training verify rank ")
        ]
        self.assertEqual(8, len(verification))
        self.assertTrue(all(line.endswith(" PASS") for line in verification))

        accepted = text.count("OFFLOAD ACCEPTED")
        fallback = text.count("SOFTWARE FALLBACK")
        self.assertEqual(8 * epochs * accepted_per_epoch, accepted)
        self.assertEqual(
            8 * epochs * (collectives_per_epoch - accepted_per_epoch), fallback
        )

        nic_statistic = re.compile(
            r"^\s+nic(?P<rank>[0-7])\.(?P<name>collective[A-Za-z]+) : "
            r"Accumulator : Sum\.u64 = (?P<sum>[0-9]+);",
            re.MULTILINE,
        )
        nic_values = {
            (int(match.group("rank")), match.group("name")): int(match.group("sum"))
            for match in nic_statistic.finditer(text)
        }
        for rank in range(8):
            for name in (
                "collectiveEnqueued",
                "collectiveSchedulerSends",
                "collectiveResultsCompleted",
            ):
                self.assertEqual(
                    epochs * accepted_per_epoch,
                    nic_values[(rank, name)],
                    f"wrong {scenario}/{path} {name} at rank {rank}",
                )

        tree_statistic = re.compile(
            r"\.(?P<name>local_contributions|upward_aggregates|result_packets) : "
            r"Accumulator : Sum\.u64 = (?P<sum>[0-9]+);"
        )
        tree_packets = sum(
            int(match.group("sum")) for match in tree_statistic.finditer(text)
        )
        self.assertEqual(28 * epochs * accepted_per_epoch, tree_packets)
