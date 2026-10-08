import csv
import importlib.util
import io
import math
import re
import statistics
from dataclasses import dataclass, field
from pathlib import Path

from .errors import EvidenceError


MAX_ROWS = 32768
MAX_COLUMNS = 512
NUMBER = r"[-+0-9.eE]+"


def number(value):
    try:
        result = float(value)
    except (TypeError, ValueError) as exc:
        raise EvidenceError("PARSE_FAILED", "Invalid numeric evidence") from exc
    if not math.isfinite(result):
        raise EvidenceError("PARSE_FAILED", "Nonfinite evidence is not a measurement")
    return result


def fields(text):
    return dict(re.findall(r"([A-Za-z_][\w.]*)=([^\s]+)", text))


def read_csv(text):
    lines = [line for line in text.splitlines() if line and not line.startswith("#")]
    reader = csv.reader(io.StringIO("\n".join(lines)), strict=True)
    try:
        columns = next(reader)
        if not columns or len(columns) > MAX_COLUMNS or len(set(columns)) != len(columns):
            raise EvidenceError("PARSE_FAILED", "Missing, duplicate or excessive columns")
        rows = []
        for row in reader:
            if len(rows) >= MAX_ROWS or len(row) != len(columns) or (len(rows) + 1) * len(columns) > 3000000:
                raise EvidenceError("PARSE_FAILED", "Invalid row width or excessive sample count")
            rows.append(dict(zip(columns, (None if value == "" else number(value) for value in row))))
        if not rows:
            raise EvidenceError("PARSE_FAILED", "Capture contains no samples")
        return columns, rows
    except (csv.Error, StopIteration) as exc:
        raise EvidenceError("PARSE_FAILED", "Incomplete CSV capture") from exc


def load_analyzer(root):
    path = Path(root) / "tests/perf/analyze_stress.py"
    spec = importlib.util.spec_from_file_location("quakeray_stress_analyzer", path)
    if spec is None or spec.loader is None:
        raise EvidenceError("ANALYZER_UNAVAILABLE", "Repository stress analyzer is unavailable")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@dataclass
class Capture:
    format: str
    aggregation_kind: str
    columns: list
    rows: list
    metadata: dict
    metrics: dict = field(default_factory=dict)
    issues: list = field(default_factory=list)


def parse_frames(text, analyzer, budget_ms):
    columns, rows = read_csv(text)
    for row in rows:
        if any(value is None for value in row.values()):
            raise EvidenceError("PARSE_FAILED", "Per-frame numeric fields cannot be blank")
        for name in ("key_game", "paused", "ui_only", "gpu_valid"):
            if row.get(name) not in (0, 1):
                raise EvidenceError("PARSE_FAILED", "Invalid frame context flags")
        if row.get("signon") != 4:
            raise EvidenceError("PARSE_FAILED", "Capture lacks a fully loaded game")
    try:
        summary = analyzer.summarize(text, budget_ms)
    except (ValueError, KeyError, TypeError, OverflowError) as exc:
        raise EvidenceError("PARSE_FAILED", str(exc)) from exc
    metadata = fields(text.splitlines()[0])
    metrics = {key: value for key, value in summary.items()
               if isinstance(value, (int, float)) and key not in {"frames", "intervals"}}
    metrics.update(summary["cpu_mean_ms"])
    metrics.update(summary["gpu_snapshot_mean_ms"])
    if any(not math.isfinite(value) for value in metrics.values()):
        raise EvidenceError("PARSE_FAILED", "Derived frame metrics are not finite")
    return Capture("frame_csv", "per_frame", columns, rows,
                   {**metadata, "map": summary["map"], "summary": summary}, metrics)


def parse_window(text):
    header = re.search(r"^# rt_stats_dump_start (.+?) panels (\d+) interval (\S+) samples (\d+) duration (\S+)$",
                       text, re.M)
    if not header:
        raise EvidenceError("PARSE_FAILED", "Missing reporting-window metadata")
    columns, rows = read_csv(text)
    interval, duration = number(header[3]), number(header[5])
    if len(rows) != int(header[4]) or len(rows) > 700 or not 0.05 <= interval <= 0.2 or duration < 0:
        raise EvidenceError("PARSE_FAILED", "Invalid reporting-window metadata or sample count")
    if "t" not in columns or any(row["t"] is None or row["t"] < 0 for row in rows):
        raise EvidenceError("PARSE_FAILED", "Missing reporting-window clock")
    if any(rows[i]["t"] <= rows[i - 1]["t"] for i in range(1, len(rows))):
        raise EvidenceError("PARSE_FAILED", "Nonadvancing reporting-window clock")
    if abs(rows[-1]["t"] - duration) > 0.02:
        raise EvidenceError("PARSE_FAILED", "Reporting-window duration does not match samples")
    return Capture("window_csv", "reporting_window", columns, rows,
                   {"stamp": header[1], "panels": int(header[2]), "interval_s": interval, "duration_s": duration},
                   issues=["Window maxima/averages are not whole-frame percentiles"])


def parse_snapshot(text):
    starts = list(re.finditer(r"^# rt_stats_dump (.+?) panels (\d+)$", text, re.M))
    if not starts:
        raise EvidenceError("PARSE_FAILED", "Missing readout snapshot metadata")
    head = starts[-1]
    row = {}
    unknown = []
    gpu_valid = None
    for line in text[head.end():].splitlines():
        if not line.strip():
            continue
        match = re.match(rf"^(cpu\.draw)\s+(.+?)\s+avg_ms=({NUMBER})\s+max_ms=({NUMBER})$", line)
        if match:
            name = match[2].strip().replace(" ", "_")
            row[f"cpu.draw.{name}_avg_ms"] = number(match[3])
            row[f"cpu.draw.{name}_max_ms"] = number(match[4])
            continue
        match = re.match(rf"^(gpu\.frame|gpu\.pass|cpu\.frame|cpu\.main|cpu\.wait|cpu\.slot|cpu\.avg|gpu\.rays|gpu\.calls|cpu\.cluster|fps)\s+(.+?)\s+({NUMBER})$", line)
        if match:
            section, name, value = match.groups()
            name = name.strip().replace(" ", "_")
            if section in {"cpu.frame", "cpu.main", "cpu.wait", "gpu.frame"}:
                key = f"{section}_ms"
            elif section == "fps":
                key = "fps"
            elif section == "gpu.pass":
                gpu_valid = True
                key = f"gpu.{name}_ms"
            elif section in {"cpu.slot", "cpu.avg"}:
                key = f"{section}.{name}_ms"
            else:
                key = f"{section}.{name}"
            if key in row:
                raise EvidenceError("PARSE_FAILED", "Duplicate snapshot field")
            row[key] = number(value)
        elif "unavailable" in line:
            unknown.append(line)
            if line.startswith("gpu"):
                gpu_valid = False
        else:
            unknown.append(line)
    if not row:
        raise EvidenceError("PARSE_FAILED", "Snapshot contains no numeric readings")
    if gpu_valid is not True:
        for key in list(row):
            if key.startswith("gpu.") and key.endswith("_ms"):
                row[key] = None
    return Capture("snapshot", "readout_snapshot", list(row), [row],
                   {"stamp": head[1], "panels": int(head[2]), "gpu_valid": gpu_valid, "unparsed": unknown,
                    "snapshots_in_file": len(starts), "selected_snapshot": len(starts) - 1},
                   issues=["A readout snapshot cannot yield frame percentiles"])


def parse_stats(text, analyzer, budget_ms):
    if text.startswith("# rt_bench_frames"):
        return parse_frames(text, analyzer, budget_ms)
    if text.startswith("# rt_stats_dump_start"):
        return parse_window(text)
    return parse_snapshot(text)


def metric_summaries(capture, analyzer):
    summaries = []
    for name in capture.columns:
        values = [row[name] for row in capture.rows if row[name] is not None and
                  (not name.startswith("gpu.") or capture.format != "frame_csv" or row["gpu_valid"] == 1)]
        if name == "interval_ms" and capture.format == "frame_csv":
            values = values[1:]
        excluded = 1 if name == "interval_ms" and capture.format == "frame_csv" else 0
        aggregation = capture.aggregation_kind
        if name.startswith("gpu."):
            aggregation = "completed_gpu_snapshot"
        elif capture.format == "window_csv" and name.startswith("cpu."):
            aggregation = "window_average" if "_avg_ms" in name else "window_maximum"
        elif capture.format == "snapshot" and name.startswith("cpu."):
            aggregation = "window_average" if name.startswith("cpu.avg.") or "_avg_ms" in name else "window_maximum"
        unit = "fps" if name == "fps" else "ms" if name.endswith("_ms") else "value"
        summaries.append({
            "name": name,
            "unit": unit,
            "aggregation_kind": aggregation,
            "sample_count": len(values),
            "unavailable_count": len(capture.rows) - len(values) - excluded,
            "excluded_count": excluded,
            "mean": statistics.fmean(values) if values else None,
            "median": statistics.median(values) if values else None,
            "p95": analyzer.percentile(values, 0.95) if values and len(values) > 1 else None,
            "max": max(values) if values else None,
        })
    return summaries


def parse_benchmarks(text):
    blocks = []
    current = None
    for line in text.splitlines():
        if line.startswith("# rt_bench "):
            metadata = fields(line)
            if not {"demo", "frames", "seconds", "fps", "interrupted"}.issubset(metadata):
                raise EvidenceError("PARSE_FAILED", "Incomplete benchmark header")
            current = {"header": metadata, "slots": {}, "settings": {}, "cluster": {}, "main": {},
                       "frame_samples": None, "unknown_lines": [], "complete": False}
            blocks.append(current)
            if len(blocks) > 1000:
                raise EvidenceError("PARSE_FAILED", "Too many benchmark blocks")
            for key in ("frames", "seconds", "fps", "interrupted"):
                current["header"][key] = number(metadata[key])
            if metadata["frames"] <= 0 or metadata["seconds"] <= 0 or metadata["interrupted"] not in (0, 1):
                raise EvidenceError("PARSE_FAILED", "Invalid benchmark metadata")
            if metadata["frames"] != int(metadata["frames"]) or metadata["fps"] < 0:
                raise EvidenceError("PARSE_FAILED", "Invalid benchmark frame count or FPS")
        elif current is not None and line.startswith("settings "):
            current["settings"] = fields(line)
        elif current is not None and line.startswith("frame_samples "):
            record = fields(line)
            if not {"file", "samples", "dropped"}.issubset(record):
                raise EvidenceError("PARSE_FAILED", "Incomplete frame-sample association")
            for key in ("samples", "dropped"):
                record[key] = number(record[key])
                if record[key] < 0 or record[key] != int(record[key]):
                    raise EvidenceError("PARSE_FAILED", "Invalid frame-sample counts")
            current["frame_samples"] = record
        elif current is not None and line.startswith("cpu.main "):
            match = re.match(rf"^cpu\.main\s+(.+?)\s+avg_ms=({NUMBER})$", line)
            if not match:
                raise EvidenceError("PARSE_FAILED", "Invalid CPU main record")
            current["main"][match[1]] = number(match[2])
        elif current is not None and line.startswith("cpu.cluster "):
            current["cluster"] = {k: number(v) for k, v in fields(line).items()}
        elif current is not None and line.startswith("cpu.slot "):
            match = re.match(rf"^cpu\.slot\s+(.+?)\s+avg_ms=({NUMBER})\s+max_ms=({NUMBER})$", line)
            if not match or match[1] in current["slots"]:
                raise EvidenceError("PARSE_FAILED", "Invalid or duplicate CPU slot")
            current["slots"][match[1]] = {"avg_ms": number(match[2]), "max_ms": number(match[3])}
        elif current is not None and line.strip():
            current["unknown_lines"].append(line)
    if not blocks:
        raise EvidenceError("PARSE_FAILED", "No benchmark blocks found")
    for block in blocks:
        record = block["frame_samples"]
        block["complete"] = bool(block["settings"] and block["slots"] and record and
                                 record["samples"] == block["header"]["frames"] and record["dropped"] == 0 and
                                 block["header"]["interrupted"] == 0)
        block["acceptance"] = "not_checked"
    return blocks
