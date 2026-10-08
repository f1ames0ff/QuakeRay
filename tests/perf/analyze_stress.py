import argparse
import csv
import io
import json
import math
from pathlib import Path
import re
import statistics


def percentile(values, fraction):
    ordered = sorted(values)
    if not ordered:
        raise ValueError("Cannot compute a percentile without samples")
    position = (len(ordered) - 1) * fraction
    low = math.floor(position)
    high = math.ceil(position)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def parse_capture(text, allow_paused=False):
    metadata = re.search(r"^# rt_bench_frames demo=(\S+) frames=(\d+) samples=(\d+) dropped=(\d+)$", text, re.MULTILINE)
    if not metadata:
        raise ValueError("Missing per-frame capture metadata")
    if int(metadata[4]) != 0:
        raise ValueError("Capture dropped frame samples")
    rows = list(csv.DictReader(io.StringIO("\n".join(line for line in text.splitlines() if not line.startswith("#")))))
    if len(rows) != int(metadata[2]) or len(rows) != int(metadata[3]) or len(rows) < 5:
        raise ValueError("Frame count does not match the complete capture")
    required = {"frame", "time_ms", "interval_ms", "client_time", "key_game", "paused", "signon", "ui_only", "gpu_valid"}
    if not required.issubset(rows[0]):
        raise ValueError("Capture lacks required timing/context columns")
    for index, row in enumerate(rows):
        if int(row["frame"]) != index:
            raise ValueError("Non-sequential frame IDs")
        if int(row["signon"]) != 4 or int(row["ui_only"]) != 0:
            raise ValueError("Capture did not render a fully loaded game")
        if not allow_paused and (int(row["key_game"]) != 1 or int(row["paused"]) != 0):
            raise ValueError("Capture ran with a menu, console, or paused simulation")
        for key, value in row.items():
            number = float(value)
            if not math.isfinite(number) or (key.endswith("_ms") and number < 0.0):
                raise ValueError(f"Invalid numeric sample in {key}")
        if index:
            elapsed = float(row["time_ms"]) - float(rows[index - 1]["time_ms"])
            if abs(elapsed - float(row["interval_ms"])) > 0.001:
                raise ValueError("Frame interval does not match the monotonic capture clock")
    if float(rows[0]["interval_ms"]) != 0.0:
        raise ValueError("The partial first frame must not have a full interval")
    if not allow_paused and float(rows[-1]["client_time"]) <= float(rows[0]["client_time"]):
        raise ValueError("Simulation time did not advance")
    return metadata[1], rows


def summarize(text, budget_ms, allow_paused=False):
    map_name, rows = parse_capture(text, allow_paused)
    intervals = [float(row["interval_ms"]) for row in rows[1:]]
    mean = statistics.fmean(intervals)
    if mean <= 0.0:
        raise ValueError("Capture has no positive frame interval")
    cpu = {}
    gpu = {}
    for key in rows[0]:
        if key.startswith("cpu.") and key.endswith("_ms"):
            cpu[key] = statistics.fmean(float(row[key]) for row in rows)
        if key.startswith("gpu.") and key.endswith("_ms"):
            values = [float(row[key]) for row in rows if int(row["gpu_valid"])]
            if values:
                gpu[key] = statistics.fmean(values)
    return {
        "map": map_name,
        "frames": len(rows),
        "intervals": len(intervals),
        "fps": 1000.0 / mean,
        "mean_ms": mean,
        "p50_ms": percentile(intervals, 0.50),
        "p90_ms": percentile(intervals, 0.90),
        "p95_ms": percentile(intervals, 0.95),
        "p99_ms": percentile(intervals, 0.99),
        "max_ms": max(intervals),
        "budget_ms": budget_ms,
        "over_budget_percent": 100.0 * sum(value > budget_ms for value in intervals) / len(intervals),
        "cpu_mean_ms": cpu,
        "gpu_snapshot_mean_ms": gpu,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("captures", nargs="+", type=Path)
    parser.add_argument("--budget-ms", type=float)
    parser.add_argument("--json", type=Path)
    parser.add_argument("--allow-paused", action="store_true")
    arguments = parser.parse_args()
    files = []
    for path in arguments.captures:
        files.extend(sorted(path.glob("*.frames.csv")) if path.is_dir() else [path])
    if not files:
        parser.error("No per-frame captures were found")
    results = []
    try:
        for path in files:
            budget = arguments.budget_ms or (1000.0 / 45.0 if "quality" in path.name else 1000.0 / 60.0)
            result = summarize(path.read_text(encoding="utf-8-sig"), budget, arguments.allow_paused)
            result["file"] = str(path)
            results.append(result)
            print(f"{path.name}: {result['fps']:.1f} FPS, mean {result['mean_ms']:.2f} ms, "
                  f"p95 {result['p95_ms']:.2f}, p99 {result['p99_ms']:.2f}, max {result['max_ms']:.2f}, "
                  f"over budget {result['over_budget_percent']:.1f}%")
        if arguments.json:
            arguments.json.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"Invalid capture: {error}\n")


if __name__ == "__main__":
    main()
