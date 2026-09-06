#!/usr/bin/env python3
"""Benchmark regression gate (ADR 0015).

Compares one or more benchmark JSON results (emitted by the harness in
backend/include/benchmark/harness.hpp) against per-metric ceilings in
docs/benchmarks/thresholds.json and fails on regression.

Usage:
  python scripts/check_benchmarks.py benchmarks_out/matching.json [more.json ...]
  python scripts/check_benchmarks.py --thresholds docs/benchmarks/thresholds.json benchmarks_out/*.json

Threshold file schema (ceilings in ns; a case missing from the file is
reported as UNCOVERED but does not fail, so new benchmarks can land before
their baseline is recorded):

{
  "note": "...",
  "cases": {
    "<case name>": {"p50_ns": 1000, "p99_ns": 5000, "p999_ns": 100000}
  }
}

Ceilings are deliberately generous at first (~2x baseline) and tighten as
variance data accumulates — a noisy gate that gets ignored is worse than none.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

COMPARABLE_METRICS = ("p50_ns", "p95_ns", "p99_ns", "p999_ns", "max_ns")


def load_json(path: Path) -> dict:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", nargs="+", help="benchmark JSON files to check")
    parser.add_argument(
        "--thresholds",
        default="docs/benchmarks/thresholds.json",
        help="threshold file (default: docs/benchmarks/thresholds.json)",
    )
    args = parser.parse_args()

    thresholds_path = Path(args.thresholds)
    if not thresholds_path.exists():
        print(f"ERROR: thresholds file not found: {thresholds_path}")
        return 2
    thresholds = load_json(thresholds_path).get("cases", {})

    failures: list[str] = []
    uncovered: list[str] = []
    checked = 0

    for result_arg in args.results:
        result_path = Path(result_arg)
        if not result_path.exists():
            failures.append(f"{result_path}: result file not found")
            continue
        payload = load_json(result_path)
        suite = payload.get("suite", result_path.stem)

        for case in payload.get("cases", []):
            name = case.get("name", "<unnamed>")
            limits = thresholds.get(name)
            if limits is None:
                uncovered.append(f"{suite}/{name}")
                continue

            for metric in COMPARABLE_METRICS:
                if metric not in limits:
                    continue
                ceiling = limits[metric]
                actual = case.get(metric)
                if actual is None:
                    failures.append(f"{suite}/{name}: metric {metric} missing from result")
                    continue
                checked += 1
                if actual > ceiling:
                    failures.append(
                        f"{suite}/{name}: {metric}={actual}ns exceeds ceiling {ceiling}ns"
                    )

    for entry in uncovered:
        print(f"UNCOVERED (no threshold recorded yet): {entry}")

    if failures:
        print(f"\nbenchmark regression check: {len(failures)} FAILURE(S) ({checked} metrics checked)")
        for failure in failures:
            print(f"  {failure}")
        return 1

    print(f"benchmark regression check: OK ({checked} metrics checked)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
