import hashlib
import json
from pathlib import Path
import uuid
import re

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
        identity = {}
        verified = False
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
            manifest = json.loads(manifest_raw.decode("utf-8-sig"))
            provenance.update(effective_settings=block["settings"],
                              manifest=manifest,
                              benchmark_sha256=hashlib.sha256(bench_raw).hexdigest(),
                              manifest_sha256=hashlib.sha256(manifest_raw).hexdigest())
            build_receipt = path.parent.parent / 'qray-build.json'
            if build_receipt.is_file():
                receipt = json.loads(bounded_bytes(build_receipt, 1024 * 1024).decode('utf-8-sig'))
                if receipt.get('ExecutableSha256') == manifest.get('ExecutableSha256') and receipt.get('EngineAssetsSha256') == manifest.get('EngineAssetsSha256'):
                    provenance['build_receipt'] = receipt
                    provenance['binary_identity'] = 'build_receipt_matched'
            runtime = path.parent.parent
            def file_hash(file):
                digest = hashlib.sha256()
                with file.open('rb') as stream:
                    for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                        digest.update(chunk)
                return digest.hexdigest().upper()
            valid_assets = True
            assets = {}
            for asset in manifest.get('Assets', []):
                name = asset.get('File', '')
                if not name or Path(name).name != name or '/' in name or '\\' in name:
                    raise EvidenceError('PERMISSION_DENIED', 'Unsafe manifest asset name')
                file = (runtime / 'ad' / name).resolve()
                if not file.is_relative_to(directory) or not file.is_file() or file_hash(file) != asset.get('Sha256'):
                    valid_assets = False
                    break
                assets[name] = asset['Sha256']
            for name in ('pak0.pak', 'pak1.pak'):
                file = runtime / 'id1' / name
                if not file.is_file():
                    valid_assets = False
                else:
                    assets['id1/' + name] = file_hash(file)
            console = path.with_name(path.name.removesuffix('.frames.csv') + '.console.log')
            console_text = bounded_bytes(console, 1024 * 1024).decode('utf-8-sig', errors='replace') if console.is_file() else ''
            hardware = re.findall(r'^(?:CPU|GPU)\s*:\s*(.+)$', console_text, re.M)
            drivers = re.findall(r'^Video\s*:\s*(.+)$', console_text, re.M)
            label = path.name.removesuffix('.frames.csv')
            checks = manifest.get('CaptureChecks', {}).get(label, {})
            focus = checks.get('FocusVerified') is True and checks.get('Completed') is True
            provenance['focus'] = 'runner_verified' if focus else 'unverified'
            bundle = hashlib.sha256(json.dumps(assets, sort_keys=True).encode()).hexdigest()
            save_hash = assets.get(request.get('save', '') + '.sav')
            identity = {'scenario_id': request.get('scenario_id'), 'save_sha256': save_hash,
                        'map_assets_fingerprint': hashlib.sha256((capture.metadata['map'] + bundle).encode()).hexdigest(),
                        'mod_assets_sha256': bundle, 'engine_assets_sha256': manifest.get('EngineAssetsSha256'),
                        'profile': 'stress_target', 'effective_settings': block['settings'], 'hardware': hardware,
                        'driver': drivers, 'build_config': provenance.get('build_receipt', {}).get('BuildConfig'),
                        'instrumentation': {'stats_level': manifest.get('StatsLevel'), 'sound_disabled': manifest.get('NoSound'),
                                            'loader_layers': manifest.get('LoaderLayers')}}
            verified = bool(valid_assets and save_hash and focus and hardware and drivers and
                            provenance['binary_identity'] == 'build_receipt_matched' and
                            file_hash(runtime / 'quakeray.exe') == manifest.get('ExecutableSha256') and
                            file_hash(runtime / 'id1/qray.pkz') == manifest.get('EngineAssetsSha256'))
        records.append({"schema_version": 1, "run_id": "run_" + uuid.uuid4().hex, "job_id": identifier,
                        "arm": "baseline" if path.is_relative_to(directory / 'baseline') else 'candidate',
                        "artifact_path": str(path.relative_to(store_root)), "sha256": hashlib.sha256(raw).hexdigest(),
                        "format": capture.format, "aggregation_kind": capture.aggregation_kind,
                        "metrics": capture.metrics, "provenance": provenance, "validity": "captured_not_accepted",
                        "provenance_verified": verified, "identity": identity, "map": capture.metadata.get('map'),
                        "visual": "not_checked", "build_config": identity.get('build_config', 'unknown')})
        if verified:
            records[-1]['validity'] = 'valid'
    if len(json.dumps(records, allow_nan=False).encode()) > 4 * 1024 * 1024:
        raise EvidenceError("FILE_TOO_LARGE", "Capture-record batch exceeds the catalog bound")
    return records
