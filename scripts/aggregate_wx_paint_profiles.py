#!/usr/bin/env python3
"""Aggregate repeated sakura-wx-paint-benchmark JSON runs."""

from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path


SCENARIOS = (
    "full-ascii",
    "large-screen",
    "partial-unicode",
    "glyph-cache-churn",
    "scroll",
)
METRICS = (
    "p50_paint_us",
    "p95_paint_us",
    "p99_paint_us",
    "glyph_cache_bytes",
    "glyph_cache_peak_bytes",
    "glyph_cache_bypasses",
    "glyph_spans",
    "glyph_cache_evictions",
)


def median(values: list[int]) -> int | float:
    value = statistics.median(values)
    if isinstance(value, int):
        return value
    return int(value) if value.is_integer() else value


def aggregate(files: list[Path]) -> dict:
    groups: dict[tuple[int, int], dict[str, list[dict[str, int]]]] = {}
    for path in files:
        document = json.loads(path.read_text(encoding="utf-8"))
        if document.get("invariants_passed") is not True:
            raise ValueError(f"benchmark invariants failed: {path}")
        scenarios = {
            item["name"]: item for item in document.get("scenarios", [])
        }
        full_ascii = scenarios.get("full-ascii")
        if full_ascii is None:
            raise ValueError(f"missing full-ascii scenario: {path}")
        cache_bytes = int(full_ascii["glyph_cache_max_bytes"])
        run_cells = int(full_ascii["glyph_cache_max_run_cells"])
        group = groups.setdefault((cache_bytes, run_cells), {})
        for name in SCENARIOS:
            item = scenarios.get(name)
            if item is None:
                raise ValueError(f"missing {name} scenario: {path}")
            group.setdefault(name, []).append(item["metrics"])

    result = {
        "benchmark": "sakura-wx-paint-profile-summary",
        "runs": len(files),
        "cache_sizes": [],
    }
    for cache_bytes, run_cells in sorted(groups):
        cache_result = {
            "cache_bytes": cache_bytes,
            "run_cells": run_cells,
            "runs": len(next(iter(groups[(cache_bytes, run_cells)].values()))),
            "scenarios": {},
        }
        for name, samples in groups[(cache_bytes, run_cells)].items():
            cache_result["scenarios"][name] = {
                metric: median([int(sample[metric]) for sample in samples])
                for metric in METRICS
            }
        result["cache_sizes"].append(cache_result)
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--pattern", default="wx-paint-run-*.json")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    files = sorted(args.input_dir.glob(args.pattern))
    if not files:
        parser.error(f"no profile files matched {args.pattern!r}")
    summary = aggregate(files)
    args.output.write_text(json.dumps(summary, indent=2) + "\n",
                           encoding="utf-8")
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
