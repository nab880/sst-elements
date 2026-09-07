#!/usr/bin/env python3
"""Run software/offload pairs and print their simulated training-step speedup."""

import argparse
from pathlib import Path
import re
import subprocess


SUMMARY = re.compile(
    r"AITraining rank (?P<rank>\d+): ranks (?P<ranks>\d+), "
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
        raise RuntimeError(f"AITraining summaries missing from {scenario}/{path} output")
    ranks = int(matches[0].group("ranks"))
    if len(matches) != ranks:
        raise RuntimeError(
            f"expected {ranks} AITraining summaries from {scenario}/{path}, "
            f"found {len(matches)}"
        )
    return {
        name: max(float(match.group(name)) for match in matches)
        for name in ("step", "collectives")
    }


def main():
    parser = argparse.ArgumentParser(
        description="Compare software and in-network paths for the Ember AI demo"
    )
    parser.add_argument("--sst", default="sst", help="SST executable")
    parser.add_argument("--epochs", type=int, default=8)
    parser.add_argument("--compute-ns", type=int, default=500)
    args = parser.parse_args()

    model = Path(__file__).with_name("data_parallel_training.py")
    print("Illustrative SST model result; not a hardware performance benchmark.")
    print("Times below are the slowest worker's per-epoch simulated time.")
    print("scenario  software collective  offload collective  speedup  step speedup")
    for scenario in ("scalar", "hybrid"):
        software = run(args.sst, model, scenario, "software", args.epochs, args.compute_ns)
        offload = run(args.sst, model, scenario, "offload", args.epochs, args.compute_ns)
        print(
            f"{scenario:8}  {software['collectives']:8.3f} us       "
            f"{offload['collectives']:8.3f} us  "
            f"{software['collectives'] / offload['collectives']:7.2f}x  "
            f"{software['step'] / offload['step']:7.2f}x"
        )


if __name__ == "__main__":
    main()
