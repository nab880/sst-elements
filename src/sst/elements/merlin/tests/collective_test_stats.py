# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
"""Read one final, unindexed accumulator record per component/statistic.

SST appends _rank before the CSV extension for MPI runs. Unused ranks may
produce no file or an empty file. Callers must assert the expected record keys
so missing active-rank data fails just like any other missing statistic.
"""

import csv
from pathlib import Path


def read_statistics(path, num_ranks):
    if num_ranks < 1:
        raise ValueError("num_ranks must be positive")
    path = Path(path)
    statistics = {}
    expected_header = None
    for rank in range(num_ranks):
        rank_file = path if num_ranks == 1 else path.with_name(
            f"{path.stem}_{rank}{path.suffix}")
        if not rank_file.is_file() or rank_file.stat().st_size == 0:
            continue
        with rank_file.open(newline="", encoding="utf-8") as stream:
            reader = csv.DictReader(stream, skipinitialspace=True)
            header = [field.strip() for field in reader.fieldnames or ()]
            required = {"ComponentName", "StatisticName", "StatisticSubId", "StatisticType", "Rank"}
            if not required.issubset(header) or len(set(header)) != len(header):
                raise AssertionError(f"Invalid statistics header: {rank_file}")
            for raw in reader:
                # Unused ranks can emit only the common header, with no
                # accumulator columns. Require those columns when data exists.
                if "Sum.u64" not in header:
                    raise AssertionError(f"Missing accumulator columns: {rank_file}")
                if expected_header is not None and header != expected_header:
                    raise AssertionError(f"Inconsistent statistics header: {rank_file}")
                expected_header = header
                if None in raw or any(value is None for value in raw.values()):
                    raise AssertionError(f"Incomplete statistics row: {rank_file}")
                row = {key.strip(): value.strip() for key, value in raw.items()}
                if row["StatisticType"] != "Accumulator":
                    raise AssertionError(f"Unexpected statistic type: {row}")
                if int(row["Rank"]) != rank or row["StatisticSubId"]:
                    raise AssertionError(f"Unexpected rank or indexed statistic: {row}")
                key = (row["ComponentName"], row["StatisticName"])
                if not all(key) or key in statistics:
                    raise AssertionError(f"Empty or duplicate statistic: {key}")
                if int(row["Sum.u64"]) < 0:
                    raise AssertionError(f"Negative unsigned statistic: {key}")
                statistics[key] = row
    return statistics
