#!/usr/bin/env python3
# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
"""Run bounded mixed-traffic experiments; retain raw evidence and check progress."""

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys


ENDPOINTS = (1, 2, 3, 5, 6, 7)
DEFAULTS = dict(mode="active", arbiter="lru", shared_ingress=1,
                ingress_bandwidth=1, ingress_width=1, queue_depth=1,
                load=0.5, buffer_bytes=128, packet_bytes=64,
                iterations=100, compute_ns=0, duration_us=200, sample_us=5)
TIME_NS = {"s": 1e9, "ms": 1e6, "us": 1e3, "ns": 1, "ps": 1e-3}


def provenance(sst_executable, model):
    """Identify built binaries as well as source state, including uncommitted fixes."""
    result = dict(host=platform.platform(), python=sys.version, source_repositories={}, binary_sha256={})
    elements = model.parents[5]
    for repository in (elements, elements.parent / "sst-core"):
        head = subprocess.run(["git", "-C", str(repository), "rev-parse", "HEAD"],
                              text=True, capture_output=True)
        if head.returncode == 0:
            diff = subprocess.run(["git", "-C", str(repository), "diff", "--name-only"],
                                  text=True, capture_output=True)
            result["source_repositories"][repository.name] = dict(
                head=head.stdout.strip(), modified_tracked_files=diff.stdout.splitlines())
    executable = shutil.which(sst_executable)
    if executable:
        executable = Path(executable).resolve()
        prefix = executable.parent.parent
        candidates = [executable, prefix / "libexec" / "sstsim.x"]
        candidates += [prefix / "lib" / "sst-elements-library" / ("lib" + name + ".so")
                       for name in ("merlin", "ember", "firefly")]
        for binary in candidates:
            if binary.is_file():
                result["binary_sha256"][str(binary)] = hashlib.sha256(binary.read_bytes()).hexdigest()
    return result


def time_ns(value, unit):
    return float(value) * TIME_NS[unit]


def read_statistics(path):
    with path.open(newline="") as stream:
        return [{key.strip(): value.strip() for key, value in row.items()}
                for row in csv.DictReader(stream, skipinitialspace=True)]


def read_collective_window(path):
    windows = {}
    for line in path.read_text().splitlines():
        fields = line.split()
        if len(fields) == 8 and fields[3] == "Allreduce":
            windows[int(fields[1])] = (time_ns(fields[4], fields[5]),
                                       time_ns(fields[6], fields[7]))
    if set(windows) != {0, 1}:
        raise ValueError("both ranks must complete the Allreduce motif")
    # Intersection: every selected sample belongs to both ranks' motif windows.
    return max(pair[0] for pair in windows.values()), min(pair[1] for pair in windows.values())


def extract_metrics(case, directory):
    stdout = (directory / "stdout.txt").read_text()
    rows = read_statistics(directory / "statistics.csv")
    end = re.search(r"Simulation is complete, simulated time:\s*([\d.eE+-]+)\s*(\w+)", stdout)
    if not end:
        raise ValueError("simulation did not report completion")
    duration_ns = time_ns(*end.groups())
    if duration_ns > case["duration_us"] * 1000 + 0.001:
        raise ValueError("collective failed to finish within the ordinary measurement duration")
    ordinary_rows = [row for row in rows if re.match(r"(?:offered_load|empty_node)_\d+:networkIF$", row["ComponentName"])]
    canonical = json.dumps(ordinary_rows, sort_keys=True, separators=(",", ":")).encode()
    result = dict(case, simulation_ns=duration_ns,
                  ordinary_statistics_sha256=hashlib.sha256(canonical).hexdigest())
    scheduled_latency = None
    source_backlog = False
    for match in re.finditer(r"^\s*([\d.]+)\s+([\d.eE+-]+)\s+(ps|ns|us|ms|s)(\*)?\s*$", stdout, re.MULTILINE):
        if abs(float(match[1]) - case["load"]) < 0.005:
            scheduled_latency = time_ns(match[2], match[3])
            source_backlog = bool(match[4])
    result["ordinary_scheduled_mean_latency_ns"] = scheduled_latency
    result["ordinary_source_backlog"] = source_backlog
    active = case["mode"] == "active"
    if active:
        verified = re.findall(r"Ember Allreduce verify rank (\d+) .* PASS", stdout)
        accepted = re.findall(r"Firefly Allreduce rank (\d+) OFFLOAD ACCEPTED", stdout)
        if sorted(verified) != ["0", "1"]:
            raise ValueError("both ranks must verify the collective result")
        if any(accepted.count(str(rank)) != case["iterations"] for rank in (0, 1)):
            raise ValueError("every scalar invocation must use offload at both ranks")
        average = re.search(r"Allreduce: ranks 2, loop (\d+), 1 double\(s\), latency ([\d.eE+-]+) us", stdout)
        if not average or int(average[1]) != case["iterations"]:
            raise ValueError("missing scalar collective latency result")
        result["collective_latency_ns"] = float(average[2]) * 1000 - case["compute_ns"]
        result["collective_completions"] = len(accepted) // 2
        window_start, window_end = read_collective_window(directory / "motifs.log")
    else:
        if "OFFLOAD ACCEPTED" in stdout or "Ember Allreduce verify" in stdout:
            raise ValueError("ordinary baseline unexpectedly executed a collective")
        result["collective_latency_ns"] = None
        result["collective_completions"] = 0
        window_start, window_end = 0, duration_ns
    result["overlap_window_ns"] = [window_start, window_end]

    service = {}
    for row in rows:
        if row["StatisticName"].startswith("network_service_"):
            key = (row["ComponentName"], row["StatisticName"])
            service[key] = int(row["Sum.u64"])
    result["service_counters"] = {}
    for (_, name), value in service.items():
        result["service_counters"][name] = result["service_counters"].get(name, 0) + value
    for statistic in ("network_service_accept", "network_service_synthetic"):
        expected = 6 * case["iterations"] if active else 0
        if result["service_counters"].get(statistic, 0) != expected:
            raise ValueError("{} must equal {}".format(statistic, expected))

    endpoints = {}
    total_packets = total_latency = overlap_packets = overlap_latency = 0
    for endpoint in ENDPOINTS:
        records = [row for row in ordinary_rows
                   if row["ComponentName"].split(":")[0].endswith("_{}".format(endpoint))
                   and row["StatisticName"] == "packet_latency"]
        # End-of-simulation statistics can repeat the final periodic timestamp.
        records = sorted({int(row["SimTime"]): row for row in records}.items())
        if not records:
            raise ValueError("missing statistics for ordinary endpoint {}".format(endpoint))
        bins = []
        previous_time = previous_count = previous_sum = 0
        for ticks, row in records:
            count, latency = int(row["Count.u64"]), int(row["Sum.u64"])
            timestamp = ticks / 1000  # Model core timebase is exactly 1 ps.
            if count < previous_count or latency < previous_sum:
                raise ValueError("expected cumulative, non-resetting statistics")
            if previous_time >= window_start and timestamp <= window_end and timestamp > previous_time:
                bins.append(dict(start_ns=previous_time, end_ns=timestamp,
                                 packets=count - previous_count,
                                 latency_sum_ns=latency - previous_sum))
            previous_time, previous_count, previous_sum = timestamp, count, latency
        if case["load"] and (not bins or any(item["packets"] == 0 for item in bins)):
            raise ValueError("ordinary endpoint {} made no progress in an overlap sample".format(endpoint))
        count = previous_count
        measured_ns = sum(item["end_ns"] - item["start_ns"] for item in bins)
        overlap_count = sum(item["packets"] for item in bins)
        overlap_sum = sum(item["latency_sum_ns"] for item in bins)
        endpoints[str(endpoint)] = dict(
            received_packets=count,
            mean_latency_ns=previous_sum / count if count else None,
            maximum_latency_ns=int(records[-1][1]["Max.u64"]) if count else None,
            throughput_GBps=count * case["packet_bytes"] / duration_ns,
            overlap_packets=overlap_count,
            overlap_mean_latency_ns=overlap_sum / overlap_count if overlap_count else None,
            overlap_throughput_GBps=overlap_count * case["packet_bytes"] / measured_ns if measured_ns else 0,
            minimum_overlap_sample_packets=min((item["packets"] for item in bins), default=0),
            overlap_samples=bins)
        total_packets += count
        total_latency += previous_sum
        overlap_packets += overlap_count
        overlap_latency += overlap_sum
    if case["load"] == 0 and total_packets:
        raise ValueError("zero offered load unexpectedly received ordinary traffic")
    result["ordinary_endpoints"] = endpoints
    result["ordinary_received_packets"] = total_packets
    result["ordinary_mean_latency_ns"] = total_latency / total_packets if total_packets else None
    result["ordinary_throughput_GBps"] = total_packets * case["packet_bytes"] / duration_ns
    result["ordinary_overlap_packets"] = overlap_packets
    result["ordinary_overlap_mean_latency_ns"] = overlap_latency / overlap_packets if overlap_packets else None
    result["ordinary_overlap_throughput_GBps"] = sum(ep["overlap_throughput_GBps"] for ep in endpoints.values())
    result["minimum_endpoint_overlap_sample_packets"] = min(ep["minimum_overlap_sample_packets"] for ep in endpoints.values())
    return result


def experiment_cases(quick=False):
    cases = []

    def add(name, **kwargs):
        cases.append(dict(DEFAULTS, name=name, **kwargs))

    loads = (0.5,) if quick else (0.1, 0.5, 0.9)
    for arbiter in ("lru",):
        for load in loads:
            for mode in ("disabled", "dormant"):
                add("{}-{}-load{}".format(mode, arbiter, load), mode=mode, arbiter=arbiter, load=load)
            for shared in (0, 1):
                add("active-{}-load{}-shared{}".format(arbiter, load, shared),
                    arbiter=arbiter, load=load, shared_ingress=shared)
        for shared in (0, 1):
            add("active-{}-load0-shared{}".format(arbiter, shared),
                arbiter=arbiter, load=0, shared_ingress=shared)
            for variant, updates in (("depth2", {"queue_depth": 2}),
                                     ("buffer512", {"buffer_bytes": 512}),
                                     ("ingress4", {"ingress_bandwidth": 4})):
                if quick and variant != "depth2":
                    continue
                add("active-{}-load{}-shared{}-{}".format(arbiter, loads[-1], shared, variant),
                    arbiter=arbiter, load=loads[-1], shared_ingress=shared, **updates)
    return cases


def check_baselines(results):
    comparisons = []
    for disabled in results:
        if disabled["mode"] != "disabled":
            continue
        dormant = next(result for result in results
                       if result["mode"] == "dormant" and result["arbiter"] == disabled["arbiter"]
                       and result["load"] == disabled["load"])
        equal = disabled["ordinary_statistics_sha256"] == dormant["ordinary_statistics_sha256"]
        comparisons.append(dict(disabled=disabled["name"], dormant=dormant["name"], exact_match=equal))
        if not equal:
            raise ValueError("disabled/dormant ordinary statistics differ for {}".format(disabled["name"]))
    return comparisons


def write_summary(directory, results, metadata, failures):
    comparisons = []
    try:
        comparisons = check_baselines(results)
    except (ValueError, StopIteration) as error:
        failures.append("baseline comparison failed: {}".format(error))
    document = dict(metadata=metadata, baseline_comparisons=comparisons,
                    failures=failures, experiments=results)
    (directory / "results.json").write_text(json.dumps(document, indent=2) + "\n")
    fields = ["name", "mode", "arbiter", "load", "shared_ingress", "ingress_bandwidth",
              "queue_depth", "buffer_bytes", "collective_latency_ns", "collective_completions",
              "ordinary_mean_latency_ns", "ordinary_throughput_GBps",
              "ordinary_scheduled_mean_latency_ns", "ordinary_source_backlog",
              "ordinary_overlap_mean_latency_ns", "ordinary_overlap_throughput_GBps",
              "minimum_endpoint_overlap_sample_packets"]
    with (directory / "results.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(results)
    lines = ["# Mixed ordinary/collective traffic", "",
             "{} cases completed; {} failures. Disabled/dormant exact matches: {}.".format(
                 len(results), len(failures), len(comparisons)), "",
             "Ordinary latency and throughput below use complete sample intervals inside both ranks' Allreduce motif windows (the whole run for baselines). Throughput is aggregate decimal GB/s. A positive minimum shows observed progress at every ordinary endpoint in every selected interval; it is not a proof of starvation freedom.", "",
             "| Case | Collective ns | Ordinary ns | Ordinary GB/s | Min packets/endpoint/sample |",
             "|---|---:|---:|---:|---:|"]
    for result in results:
        def number(value):
            return "—" if value is None else "{:.3f}".format(value)
        lines.append("| {} | {} | {} | {} | {} |".format(
            result["name"], number(result["collective_latency_ns"]),
            number(result["ordinary_overlap_mean_latency_ns"]),
            number(result["ordinary_overlap_throughput_GBps"]),
            result["minimum_endpoint_overlap_sample_packets"]))
    if failures:
        lines.extend(["", "Failures:", ""] + ["- " + failure for failure in failures])
    lines.extend(["", "Raw stdout, stderr, cumulative statistics, motif logs, and command/configuration are stored per case. See results.json for each endpoint and every overlap sample.", ""])
    (directory / "summary.md").write_text("\n".join(lines))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sst", default="sst")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--quick", action="store_true", help="8-case reduced matrix")
    parser.add_argument("--timeout", type=float, default=60, help="wall-clock seconds per simulation")
    parser.add_argument("--iterations", type=int, default=DEFAULTS["iterations"])
    args = parser.parse_args(argv)
    output = args.output_dir.resolve()
    if output.exists() and any(output.iterdir()):
        parser.error("output directory must be empty (raw evidence is never overwritten)")
    output.mkdir(parents=True, exist_ok=True)
    model = Path(__file__).with_name("merlin_mixed_collective.py").resolve()
    env = dict(os.environ)
    # Single-rank experiments do not require Open MPI to bind a TCP listener.
    env.setdefault("OMPI_MCA_btl", "self")
    version = subprocess.run([args.sst, "--version"], text=True, capture_output=True, env=env)
    metadata = dict(command=sys.argv, sst=args.sst, sst_version=version.stdout.strip(),
                    model=str(model), model_sha256=hashlib.sha256(model.read_bytes()).hexdigest(),
                    provenance=provenance(args.sst, model),
                    timebase="1ps", ordinary_endpoints=list(ENDPOINTS),
                    topology="4,1:2", collective_endpoints=[0, 4], ordinary_pattern="logical shift by 3",
                    notes="One scalar invocation at a time; no chunking or concurrency. Finite progress is observed, not proven.")
    results, failures = [], []
    for case in experiment_cases(args.quick):
        case["iterations"] = args.iterations
        directory = output / case["name"]
        directory.mkdir()
        command = [args.sst, str(model), "--"]
        for key, value in case.items():
            if key != "name":
                command.extend(["--" + key.replace("_", "-"), str(value)])
        command.extend(["--statistics", str(directory / "statistics.csv"),
                        "--motif-log", str(directory / "motifs")])
        (directory / "command.json").write_text(json.dumps(dict(command=command, case=case), indent=2) + "\n")
        print("Running {}".format(case["name"]), flush=True)
        try:
            with (directory / "stdout.txt").open("w") as stdout, (directory / "stderr.txt").open("w") as stderr:
                completed = subprocess.run(command, stdout=stdout, stderr=stderr, env=env, timeout=args.timeout)
            if completed.returncode:
                raise ValueError("sst returned {}".format(completed.returncode))
            results.append(extract_metrics(case, directory))
        except (ValueError, OSError, subprocess.TimeoutExpired) as error:
            failures.append("{}: {}".format(case["name"], error))
            print(failures[-1], file=sys.stderr, flush=True)
    write_summary(output, results, metadata, failures)
    print("Results: {} ({} failures)".format(output / "summary.md", len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
