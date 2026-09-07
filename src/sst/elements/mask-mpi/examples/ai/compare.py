#!/usr/bin/env python3
"""Compare Mercury/Mask-MPI software and in-network AI training paths."""

import argparse
from pathlib import Path
import re
import subprocess


SUMMARY = re.compile(
    r"MercuryAITraining rank (?P<rank>\d+): ranks (?P<ranks>\d+), "
    r"epochs (?P<epochs>\d+), "
    r"parameters (?P<parameters>\d+), loss-sync (?P<loss>on|off), "
    r"step (?P<step>[0-9.]+) us, collectives (?P<collectives>[0-9.]+) us"
)


def run(sst_binary, model, scenario, path, epochs, compute_ns):
    command = [
        sst_binary,
        str(model),
        f"--model-options={scenario} {path} {epochs} {compute_ns}",
    ]
    completed = subprocess.run(command, check=True, text=True, capture_output=True)
    matches = list(SUMMARY.finditer(completed.stdout))
    if not matches:
        raise RuntimeError(
            f"MercuryAITraining summaries missing from {scenario}/{path} output"
        )
    ranks = int(matches[0].group("ranks"))
    if len(matches) != ranks:
        raise RuntimeError(
            f"expected {ranks} MercuryAITraining summaries from {scenario}/{path}, "
            f"found {len(matches)}"
        )
    expected_parameters = 1 if scenario == "scalar" else 4
    expected_loss = "off" if scenario == "scalar" else "on"
    expected_ranks = set(range(ranks))
    actual_ranks = {int(match.group("rank")) for match in matches}
    if actual_ranks != expected_ranks or any(
        int(match.group("ranks")) != ranks
        or int(match.group("epochs")) != epochs
        or int(match.group("parameters")) != expected_parameters
        or match.group("loss") != expected_loss
        for match in matches
    ):
        raise RuntimeError(
            f"inconsistent MercuryAITraining summaries from {scenario}/{path}"
        )
    return {
        name: max(float(match.group(name)) for match in matches)
        for name in ("step", "collectives")
    }


def ratio(numerator, denominator):
    return numerator / denominator if denominator else float("inf")


def main():
    parser = argparse.ArgumentParser(
        description="Compare Mercury AI training software and in-network paths"
    )
    parser.add_argument("--sst", default="sst", help="SST executable")
    parser.add_argument("--epochs", type=int, default=8)
    parser.add_argument("--compute-ns", type=int, default=500)
    args = parser.parse_args()

    model = Path(__file__).with_name("data_parallel_training.py")
    print("Illustrative SST model result; not a hardware performance benchmark.")
    print("Times below are the slowest worker's per-epoch simulated time.")
    print(
        "scenario  software collective  offload-enabled path  speedup  step speedup"
    )
    for scenario in ("scalar", "hybrid"):
        software = run(args.sst, model, scenario, "software", args.epochs, args.compute_ns)
        offload = run(args.sst, model, scenario, "offload", args.epochs, args.compute_ns)
        print(
            f"{scenario:8}  {software['collectives']:8.3f} us       "
            f"{offload['collectives']:8.3f} us  "
            f"{ratio(software['collectives'], offload['collectives']):7.2f}x  "
            f"{ratio(software['step'], offload['step']):7.2f}x"
        )


if __name__ == "__main__":
    main()
