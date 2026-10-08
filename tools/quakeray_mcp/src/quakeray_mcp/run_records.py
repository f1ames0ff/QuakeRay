import hashlib
import json
from pathlib import Path
import uuid

from .errors import EvidenceError
from .parsers import parse_benchmarks, parse_stats


def bounded_bytes(path, maximum=64 * 1024 * 1024):
    with path.open("rb") as stream:
        raw = stream.read(maximum + 1)
    if len(raw) > maximum:
        raise EvidenceError("FILE_TOO_LARGE", "Evidence exceeds its byte bound")
    return raw


def prepare_records(directory, store_root, identifier, result, analyzer):
    request_path = directory / "request.json"
    request = json.loads(bounded_bytes(request_path, 1024 * 1024).decode("utf-8")) if request_path.is_file() else {}
    paths = result.get("captures", [])
    if not isinstance(paths, list) or not 1 <= len(paths) <= 20:
        raise EvidenceError("CAPTURE_MISSING", "A bounded complete capture batch is required")
    seen = set()
    records = []
    for value in paths:
        path = Path(value).resolve()
        if not path.is_relative_to(directory) or path in seen:
            raise EvidenceError("PERMISSION_DENIED", "External or duplicate capture in worker results")
        seen.add(path)
        raw = bounded_bytes(path)
        text = raw.decode("utf-8-sig").replace("\r\n", "\n")
        preset = request.get("preset")
        if text.startswith("# rt_bench_frames") and preset not in {"balanced", "quality"}:
            raise EvidenceError("MISSING_PROVENANCE", "Per-frame capture requires an explicit preset")
        budget = 1000 / 45 if preset == "quality" else 1000 / 60
        capture = parse_stats(text, analyzer, budget)
        provenance = {"request": request, "preset": preset, "budget_ms": budget,
                      "binary_identity": "unverified", "focus": "unverified"}
        if capture.format == "frame_csv":
            bench_path = path.with_name(path.name.removesuffix(".frames.csv") + ".bench.log")
            manifest_path = path.parent / "manifest.json"
            if not bench_path.is_file() or not manifest_path.is_file():
                raise EvidenceError("MISSING_PROVENANCE", "Frame capture needs its benchmark block and manifest")
            bench_raw = bounded_bytes(bench_path, 1024 * 1024)
            blocks = parse_benchmarks(bench_raw.decode("utf-8-sig").replace("\r\n", "\n"))
            if len(blocks) != 1 or not blocks[0]["complete"]:
                raise EvidenceError("INCOMPARABLE", "Capture benchmark block is incomplete or ambiguous")
            block = blocks[0]
            fsr = "2" if preset == "quality" else "3"
            if block["settings"].get("rt_upscale_fsr31") != fsr or not block["settings"].get("vid", "").startswith("3840x2160@"):
                raise EvidenceError("INCOMPARABLE", "Effective preset does not match the requested target")
            if block["header"]["demo"] != capture.metadata["map"] or block["frame_samples"]["samples"] != len(capture.rows):
                raise EvidenceError("INCOMPARABLE", "Benchmark/frame association does not match")
            manifest_raw = bounded_bytes(manifest_path, 1024 * 1024)
            provenance.update(effective_settings=block["settings"],
                              manifest=json.loads(manifest_raw.decode("utf-8-sig")),
                              benchmark_sha256=hashlib.sha256(bench_raw).hexdigest(),
                              manifest_sha256=hashlib.sha256(manifest_raw).hexdigest())
        records.append({"schema_version": 1, "run_id": "run_" + uuid.uuid4().hex, "job_id": identifier,
                        "artifact_path": str(path.relative_to(store_root)), "sha256": hashlib.sha256(raw).hexdigest(),
                        "format": capture.format, "aggregation_kind": capture.aggregation_kind,
                        "metrics": capture.metrics, "provenance": provenance, "validity": "captured_not_accepted",
                        "provenance_verified": False, "visual": "not_checked", "build_config": "unknown"})
    if len(json.dumps(records, allow_nan=False).encode()) > 4 * 1024 * 1024:
        raise EvidenceError("FILE_TOO_LARGE", "Capture-record batch exceeds the catalog bound")
    return records
