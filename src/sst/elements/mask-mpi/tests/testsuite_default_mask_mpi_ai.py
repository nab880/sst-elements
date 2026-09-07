# -*- coding: utf-8 -*-

import re
from pathlib import Path

from sst_unittest import *


class testcase_mask_mpi_ai(SSTTestCase):

    def test_mercury_data_parallel_training_paths(self):
        test_dir = Path(self.get_testsuite_dir())
        model = test_dir.parent / "examples" / "ai" / "data_parallel_training.py"
        out_dir = Path(self.get_test_output_run_dir())
        epochs = 2

        for scenario in ("scalar", "hybrid"):
            for path in ("software", "offload"):
                with self.subTest(scenario=scenario, path=path):
                    output = out_dir / f"mercury_ai_{scenario}_{path}.out"
                    error = out_dir / f"mercury_ai_{scenario}_{path}.err"
                    self.run_sst(
                        str(model),
                        str(output),
                        str(error),
                        set_cwd=str(model.parent),
                        other_args=(
                            f'--model-options="{scenario} {path} {epochs} 0"'
                        ),
                        timeout_sec=30,
                    )
                    self._check_output(output, scenario, path, epochs)

    def _check_output(self, output, scenario, path, epochs):
        text = output.read_text(encoding="utf-8")
        parameters = 1 if scenario == "scalar" else 4
        accepted_per_epoch = 1 if path == "offload" else 0

        self.assertEqual(1, text.count("Simulation is complete"))
        summaries = [
            line
            for line in text.splitlines()
            if line.startswith("MercuryAITraining rank ")
        ]
        self.assertEqual(8, len(summaries))
        for rank in range(8):
            self.assertEqual(
                1,
                sum(
                    line.startswith(
                        f"MercuryAITraining rank {rank}: ranks 8, epochs {epochs}, "
                    )
                    and f"parameters {parameters}, " in line
                    and f"loss-sync {'off' if scenario == 'scalar' else 'on'},"
                    in line
                    for line in summaries
                ),
            )

        verification = [
            line
            for line in text.splitlines()
            if line.startswith("Mercury AI training verify rank ")
        ]
        self.assertEqual(8, len(verification))
        self.assertTrue(all(line.endswith(" PASS") for line in verification))

        tree_statistic = re.compile(
            r"\.(?P<name>local_contributions|upward_aggregates|result_packets) : "
            r"Accumulator : Sum\.u64 = (?P<sum>[0-9]+);"
        )
        tree_packets = sum(
            int(match.group("sum")) for match in tree_statistic.finditer(text)
        )
        self.assertEqual(28 * epochs * accepted_per_epoch, tree_packets)

        service_statistic = re.compile(
            r"\.(?P<name>network_service_accept|network_service_synthetic) : "
            r"Accumulator : Sum\.u64 = (?P<sum>[0-9]+);"
        )
        service_totals = {"network_service_accept": 0, "network_service_synthetic": 0}
        for match in service_statistic.finditer(text):
            service_totals[match.group("name")] += int(match.group("sum"))
        for total in service_totals.values():
            self.assertEqual(20 * epochs * accepted_per_epoch, total)
