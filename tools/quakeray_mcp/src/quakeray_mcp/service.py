from collections import OrderedDict
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import threading

from .compare import compare_records
from .errors import EvidenceError
from .parsers import load_analyzer, metric_summaries, parse_benchmarks, parse_stats
from .paths import ApprovedPaths


MAX_RESPONSE_BYTES = 32768
DOCUMENTS = {"entry": "AGENTS.md", "architecture": "ARCHITECTURE.md", "performance": "PERFORMANCE.md"}
SAVES = (
    ("qr_fuma_start", "ad_tfuma", "27D2992C64A14332E1804C139580778A7AB595C86F28FD1C39289BDF4ECC3326"),
    ("qr_ad_start", "start", "A1FCE672F441BE2938A9D9D717868A25CB87E83230552BDD23CE8F6A3BDFEE88"),
    ("qr_gpu_heavy", "ad_swampy", "F9C09773CFB386CC4BEC171661C65CA5AD1EAE0FF989B80691D8F17D06AA5C72"),
)


def digest(path):
    if not path.is_file():
        return None
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


class EvidenceService:
    def __init__(self, root, import_roots=(), enable_jobs=None, state_root=None):
        self.paths = ApprovedPaths(root, import_roots)
        self.root = self.paths.root
        self.analyzer = load_analyzer(self.root)
        self.artifacts = OrderedDict()
        self.cache_bytes = 0
        self.cache_lock = threading.RLock()
        self.requests = threading.BoundedSemaphore(2)
        self.documents = dict(DOCUMENTS)
        for path in sorted((self.root / "docs").glob("*.md")):
            self.documents[f"docs/{path.stem}"] = str(path.relative_to(self.root))
        self.jobs = None
        self.research = None
        self.code_index = None
        clangd = os.environ.get("QUAKERAY_CLANGD")
        if clangd == 'auto':
            clangd = str(Path(os.environ.get('LOCALAPPDATA', '')) / 'QuakeRayMCP/tools/clangd-23.1.0/clangd_23.1.0/bin/clangd.exe')
        if clangd:
            from .code_index import CodeIndex
            self.code_index = CodeIndex(self.root, clangd)
        enabled = os.environ.get("QUAKERAY_ENABLE_JOBS") == "1" if enable_jobs is None else enable_jobs
        if enabled:
            if os.name != "nt":
                raise EvidenceError("PROCESS_CONTAINMENT_FAILED", "Runtime jobs require Windows")
            from .jobs import JobManager
            common = self.source_identity()["git_common_directory"] or str(self.root)
            repo_id = hashlib.sha256(str((self.root / common).resolve()).encode()).hexdigest()[:24]
            state = state_root or Path(os.environ["LOCALAPPDATA"]) / "QuakeRayMCP" / repo_id / "jobs"
            self.jobs = JobManager(self.root, state)
            if os.environ.get("QUAKERAY_ENABLE_RESEARCH") == "1":
                from .research import ResearchStore
                self.research = ResearchStore(self.root, self.jobs.store)
            self.paths.roots = (*self.paths.roots, self.jobs.store.root)

    def close(self):
        if self.jobs:
            self.jobs.close()

    @property
    def tool_names(self):
        from .schemas import INPUTS, JOB_INPUTS, CODE_INPUTS, RESEARCH_INPUTS
        return sorted([*INPUTS, *(JOB_INPUTS if self.jobs else {}), *(CODE_INPUTS if self.code_index else {}),
                       *(RESEARCH_INPUTS if self.research else {})])

    def source_identity(self):
        def git(*args):
            try:
                result = subprocess.run(["git", "-C", str(self.root), *args], capture_output=True,
                                        encoding="utf-8", errors="replace", timeout=3, check=True)
                return result.stdout.strip()
            except (OSError, subprocess.SubprocessError):
                return None
        return {"head": git("rev-parse", "HEAD"), "working_changes": git("status", "--porcelain=v1"),
                "git_common_directory": git("rev-parse", "--git-common-dir")}

    def process_status(self):
        if os.name != "nt":
            return {"status": "unavailable", "instances": [], "reason": "Windows process inspection required"}
        script = "@(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match '^(quakeray|qray_|cmake$|ninja$|cl$|link$)' } | Select-Object Id,ProcessName) | ConvertTo-Json -Compress"
        try:
            result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
                                    capture_output=True, encoding="utf-8", timeout=4, check=True)
            instances = json.loads(result.stdout or "[]") or []
            return {"status": "observed", "instances": instances if isinstance(instances, list) else [instances]}
        except (OSError, subprocess.SubprocessError, ValueError):
            return {"status": "unavailable", "instances": [], "reason": "Process query failed"}

    def server_status(self):
        runtime = self.paths.runtime
        return {"phase": "P0-P5-gated" if self.research or self.code_index else "P0-P2-gated" if self.jobs else "P0-P1", "source": self.source_identity(),
                "runtime": {"id": "debug", "requested_build_config": "Debug", "verified_build_config": None,
                            "exists": runtime.is_dir(),
                            "executable_exists": (runtime / "quakeray.exe").is_file(),
                            "engine_pack_exists": (runtime / "id1/qray.pkz").is_file()},
                "processes": self.process_status(),
                "capabilities": {"read_only": not bool(self.jobs), "game_launch": bool(self.jobs), "build": bool(self.jobs),
                                 "jobs": bool(self.jobs), "experiments": bool(self.research), "semantic_index": bool(self.code_index),
                                 "persistent_run_catalog": bool(self.jobs)}}

    def list_scenarios(self, runtime_id="debug", kind="stress"):
        scenarios = []
        game = self.paths.runtime / "ad"
        for save, map_name, expected_hash in SAVES:
            actual_hash = digest(game / f"{save}.sav")
            missing = []
            if actual_hash is None:
                missing.append("save")
            elif actual_hash.upper() != expected_hash:
                missing.append("matching_save_hash")
            for name, path in (("executable", self.paths.runtime / "quakeray.exe"),
                               ("engine_pack", self.paths.runtime / "id1/qray.pkz"), ("mod_pak", game / "pak0.pak")):
                if not path.is_file():
                    missing.append(name)
            for preset, fps, fsr in (("balanced", 60, 3), ("quality", 45, 2)):
                scenarios.append({"scenario_id": f"{save}-{preset}", "save": save, "map": f"maps/{map_name}.bsp",
                                  "preset": preset, "target_fps": fps, "budget_ms": 1000 / fps,
                                  "fsr_mode": fsr, "expected_save_sha256": expected_hash,
                                  "actual_save_sha256": actual_hash, "missing": missing,
                                  "readiness": "missing_prerequisites" if missing else "identity_checks_required",
                                  "launch_supported": False})
        return {"runtime_id": runtime_id, "scenarios": scenarios, "evidence_document": "PERFORMANCE.md"}

    def import_source(self, source):
        with self.cache_lock:
            return self._import_source(source)

    def _import_source(self, source):
        if "artifact_id" in source:
            identifier = source["artifact_id"]
            if identifier not in self.artifacts:
                raise EvidenceError("NOT_FOUND", "Artifact is not in this server's bounded session cache",
                                    "Reimport the original approved file")
            self.artifacts.move_to_end(identifier)
            return self.artifacts[identifier]
        path = self.paths.resolve_import(source["import_path"])
        text, sha, byte_count = self.paths.read(path)
        identifier = "artifact_" + hashlib.sha256((str(path) + sha).encode()).hexdigest()[:32]
        if identifier in self.artifacts:
            self.artifacts.move_to_end(identifier)
            return self.artifacts[identifier]
        while self.artifacts and self.cache_bytes + byte_count > 64 * 1024 * 1024:
            _, old = self.artifacts.popitem(last=False)
            self.cache_bytes -= old["ref"]["byte_count"]
        artifact = {"path": path, "text": text, "ref": {"artifact_id": identifier, "sha256": sha,
                    "byte_count": byte_count, "name": path.name, "origin": "imported",
                    "provenance_verified": False}}
        self.artifacts[identifier] = artifact
        self.cache_bytes += byte_count
        return artifact

    def read_stats(self, source, summary=True, cursor=0, limit=20, budget_ms=1000 / 60):
        artifact = self.import_source(source)
        capture = parse_stats(artifact["text"], self.analyzer, budget_ms)
        items = metric_summaries(capture, self.analyzer) if summary else capture.rows
        metadata = {key: value for key, value in capture.metadata.items() if key != "summary"}
        result = {"artifact": artifact["ref"], "format": capture.format,
                  "aggregation_kind": capture.aggregation_kind, "metadata": metadata,
                  "schema": {"columns": capture.columns,
                             "fingerprint": hashlib.sha256(json.dumps(capture.columns).encode()).hexdigest()},
                  "validity": {"syntax": "valid", "context": "valid" if capture.format == "frame_csv" else "unknown",
                               "acceptance": "not_checked",
                               "focus": "unknown", "provenance": "unverified_import"},
                  "issues": capture.issues, "summary": capture.metrics if capture.format == "frame_csv" else {},
                  "untrusted_data": True,
                  "evidence_route": {"document": "ARCHITECTURE.md", "section": "Subsystem index"},
                  "page": {"kind": "metrics" if summary else "rows", "cursor": cursor,
                           "items": items[cursor:cursor + limit], "total": len(items), "next_cursor": None}}
        if capture.format == "frame_csv":
            result["summary"] = {k: v for k, v in capture.metrics.items() if not k.startswith(("cpu.", "gpu."))}
        while result["page"]["items"] and len(json.dumps(result).encode()) > 28000:
            result["page"]["items"].pop()
        count = len(result["page"]["items"])
        if count == 0 and cursor < len(items):
            raise EvidenceError("RESPONSE_TOO_LARGE", "A row exceeds the inline response bound")
        result["page"]["next_cursor"] = cursor + count if cursor + count < len(items) else None
        return result

    def read_benchmarks(self, source, last_n=5):
        artifact = self.import_source(source)
        blocks = parse_benchmarks(artifact["text"])
        for block in blocks[-last_n:]:
            record = block["frame_samples"]
            if not record:
                block["association_status"] = "missing"
                continue
            name = record["file"]
            if Path(name).name != name or "/" in name or "\\" in name or ":" in name:
                raise EvidenceError("PERMISSION_DENIED", "Unsafe frame-sample filename")
            target = artifact["path"].parent / name
            if not target.is_file():
                block["association_status"] = "missing_file"
                continue
            associated = self.import_source({"import_path": str(target)})
            try:
                capture = parse_stats(associated["text"], self.analyzer, 1000 / 60)
                block["association_status"] = "validated" if capture.format == "frame_csv" and len(capture.rows) == record["samples"] and capture.metadata["map"] == block["header"]["demo"] else "mismatched"
            except EvidenceError:
                block["association_status"] = "invalid_capture"
            block["associated_artifact"] = associated["ref"]
        return {"artifact": artifact["ref"], "blocks": blocks[-last_n:], "total_blocks": len(blocks),
                "untrusted_data": True}

    def compare_runs(self, baseline, candidate, policy_id="diagnostic"):
        def read(source):
            artifact = self.import_source(source)
            capture = parse_stats(artifact["text"], self.analyzer, 1000 / 60)
            metrics = dict(capture.metrics)
            if not metrics:
                metrics = {item["name"]: item["mean"] for item in metric_summaries(capture, self.analyzer)}
            return {"aggregation_kind": capture.aggregation_kind, "map": capture.metadata.get("map"),
                    "metrics": metrics, "validity": "unknown", "identity": {}, "provenance_verified": False,
                    "artifact": artifact["ref"]}
        base, cand = read(baseline), read(candidate)
        result = compare_records(base, cand)
        result["baseline_artifact"] = base["artifact"]
        result["candidate_artifact"] = cand["artifact"]
        return result

    def get_architecture(self, query="", document="", cursor=0):
        if document and document not in self.documents:
            raise EvidenceError("NOT_FOUND", "Unknown document alias")
        selected = {document: self.documents[document]} if document else self.documents
        found = []
        for alias, name in selected.items():
            path = (self.root / name).resolve()
            if not path.is_relative_to(self.root):
                raise EvidenceError("PERMISSION_DENIED", "Document escapes the repository")
            if not path.is_file():
                continue
            if path.stat().st_size > 2 * 1024 * 1024:
                raise EvidenceError("FILE_TOO_LARGE", "Documentation exceeds the 2 MiB bound")
            text, sha, byte_count = self.paths.read(path)
            if byte_count > 2 * 1024 * 1024:
                raise EvidenceError("FILE_TOO_LARGE", "Documentation exceeds the 2 MiB bound")
            if query and query.casefold() not in text.casefold():
                continue
            item = {"alias": alias, "path": name, "sha256": sha}
            if document or query:
                start = cursor
                if query and cursor == 0:
                    start = max(0, text.casefold().find(query.casefold()) - 500)
                item.update({"text": text[start:start + 5500], "cursor": start,
                             "next_cursor": start + 5500 if start + 5500 < len(text) else None})
            found.append(item)
            if query:
                break
        return {"documents": found, "source": self.source_identity(), "untrusted_data": True}

    def dispatch(self, name, arguments):
        if not self.requests.acquire(blocking=False):
            raise EvidenceError("SERVICE_BUSY", "Two evidence inspections are already running", retryable=True)
        try:
            return getattr(self, name)(**arguments)
        finally:
            self.requests.release()

    def start_build(self, runtime_id, idempotency_key, config="Debug", tests=True, parallel=4):
        if not (self.root / "third_party/nvrhi/include/nvrhi/nvrhi.h").is_file() or not (self.root / "third_party/openal-soft/CMakeLists.txt").is_file():
            raise EvidenceError("ENV_MISSING_DEPENDENCIES", "Initialize pinned build submodules before starting a build")
        arguments = {"runtime_id": runtime_id, "idempotency_key": idempotency_key,
                     "config": config, "tests": tests, "parallel": parallel}
        return self.jobs.start("build", arguments, self.jobs.worker_command("build", arguments, self.paths.runtime), 1500)

    def _start_capture(self, operation, scenario_id, runtime_id, idempotency_key, warmup_s, seconds, repeats, stats_level):
        self.jobs.runtime_health(self.paths.runtime)
        if not (self.paths.runtime / "quakeray.exe").is_file() or not (self.paths.runtime / "id1/qray.pkz").is_file():
            raise EvidenceError("ENV_MISSING_BUILD", "A complete Debug runtime is required")
        match = next((row for row in self.list_scenarios()["scenarios"] if row["scenario_id"] == scenario_id), None)
        if match is None or match["missing"]:
            raise EvidenceError("ENV_MISSING_SCENARIO", "Scenario assets/save must match the documented fixture")
        arguments = {"idempotency_key": idempotency_key, "runtime_id": runtime_id,
                     "scenario_id": scenario_id, "save": match["save"], "preset": match["preset"],
                     "warmup": warmup_s, "seconds": seconds, "repeats": repeats, "stats_level": stats_level}
        return self.jobs.start(operation, arguments, self.jobs.worker_command(operation, arguments, self.paths.runtime), 300)

    def start_benchmark(self, scenario_id, runtime_id, idempotency_key, warmup_s=8, seconds=6, repeats=1):
        return self._start_capture("benchmark", scenario_id, runtime_id, idempotency_key, warmup_s, seconds, repeats, 0)

    def start_capture(self, scenario_id, runtime_id, idempotency_key, warmup_s=8, duration_s=6):
        return self._start_capture("capture", scenario_id, runtime_id, idempotency_key, warmup_s, duration_s, 1, 3)

    def start_menu_ab(self, candidate_runtime, idempotency_key, seconds=8, smoke=False, validation=False, baseline_runtime=None):
        self.jobs.runtime_health(self.paths.runtime)
        if not (self.paths.runtime / "quakeray.exe").is_file() or not (self.paths.runtime / "id1/pak0.pak").is_file():
            raise EvidenceError("ENV_MISSING_GAME_DATA", "A complete base-game Debug runtime is required")
        arguments = {"idempotency_key": idempotency_key, "runtime_id": candidate_runtime,
                     "seconds": seconds, "smoke": smoke, "validation": validation}
        if baseline_runtime:
            configured = os.environ.get("QUAKERAY_BASELINE_RUNTIME")
            if not configured:
                raise EvidenceError("ENV_MISSING_BUILD", "Operator must register QUAKERAY_BASELINE_RUNTIME")
            baseline = Path(configured).resolve()
            self.jobs.runtime_health(baseline)
            if not (baseline / "quakeray.exe").is_file() or not (baseline / "id1/pak0.pak").is_file():
                raise EvidenceError("ENV_MISSING_GAME_DATA", "Registered baseline runtime is incomplete")
            arguments["baseline"] = str(baseline)
        return self.jobs.start("menu", arguments, self.jobs.worker_command("menu", arguments, self.paths.runtime), 300)

    def get_job(self, job_id, wait_seconds=0):
        return self.jobs.get(job_id, wait_seconds)

    def cancel_job(self, job_id, control_token):
        return self.jobs.cancel(job_id, control_token)

    def list_runs(self, last_n=20):
        return self.jobs.list_runs(last_n)

    def prune_runs(self, keep_last=20, dry_run=True):
        return self.jobs.prune(keep_last, dry_run)

    def verify_run(self, run_id):
        return self.jobs.verify_run(run_id)

    def compare_retained_runs(self, baseline_id, candidate_id):
        from .compare import compare_records
        baseline = self.jobs.reverify_record(baseline_id)
        candidate = self.jobs.reverify_record(candidate_id)
        return compare_records(baseline, candidate)

    def promote_baseline(self, run_id, name, confirm=False):
        return self.jobs.promote_baseline(run_id, name, confirm)

    def find_symbol(self, query, file="Quake/gl_rmain.c"):
        if self.process_status()["instances"]:
            raise EvidenceError("MACHINE_BUSY", "Defer AST parsing until heavy owners finish")
        return self.code_index.find(query, file)

    def get_references(self, symbol_id):
        if self.process_status()["instances"]:
            raise EvidenceError("MACHINE_BUSY", "Defer AST parsing until heavy owners finish")
        return self.code_index.related(symbol_id, "references")

    def get_callers(self, symbol_id):
        if self.process_status()["instances"]:
            raise EvidenceError("MACHINE_BUSY", "Defer AST parsing until heavy owners finish")
        return self.code_index.related(symbol_id, "incomingCalls")

    def get_callees(self, symbol_id):
        if self.process_status()["instances"]:
            raise EvidenceError("MACHINE_BUSY", "Defer AST parsing until heavy owners finish")
        return self.code_index.related(symbol_id, "outgoingCalls")

    def refresh_index(self):
        self.code_index.symbols.clear()
        return {"status": "cleared", "refresh_policy": "on_demand", "full_project_coverage": False}

    def trace_path(self, from_id, to_id, depth=4):
        if self.process_status()['instances']:
            raise EvidenceError('MACHINE_BUSY', 'Defer graph parsing until heavy owners finish')
        return self.code_index.trace(from_id, to_id, depth)

    def record_finding(self, hypothesis, subsystem, run_ids, decision="inconclusive"):
        return self.research.record_finding(hypothesis, subsystem, run_ids, decision)

    def search_findings(self, subsystem="", query="", limit=20):
        return self.research.search(subsystem, query, limit)

    def diff_screenshots(self, baseline, candidate):
        from .images import compare_png
        from .run_records import bounded_bytes
        a = self.paths.resolve_import(baseline, {'.png'})
        b = self.paths.resolve_import(candidate, {'.png'})
        return compare_png(bounded_bytes(a, 32 * 1024 * 1024), bounded_bytes(b, 32 * 1024 * 1024))

    def create_experiment(self, base_commit, name):
        return self.research.create_experiment(base_commit, name)

    def finish_experiment(self, experiment_id, decision, run_ids):
        return self.research.finish_experiment(experiment_id, decision, run_ids)

    def prepare_experiment(self, experiment_id):
        return self.research.prepare_experiment(experiment_id)

    def start_experiment_build(self, experiment_id, idempotency_key, parallel=4):
        record = self.research.read(experiment_id)
        if record['dependency_status'] != 'local_pinned_checkouts_initialized':
            raise EvidenceError('ENV_MISSING_DEPENDENCIES', 'Prepare independent experiment dependencies first')
        source = Path(record['worktree']).resolve()
        arguments = {'idempotency_key': idempotency_key, 'experiment_id': experiment_id,
                     'source_root': str(source), 'parallel': parallel, 'tests': True}
        runtime = source / 'build/Debug'
        return self.jobs.start('build', arguments, self.jobs.worker_command('build', arguments, runtime), 1500)

    def start_experiment_suite(self, experiment_id, scenario_id, idempotency_key, seconds=6, warmup_s=8, order='baseline_first'):
        record = self.research.read(experiment_id)
        candidate = Path(record['worktree']).resolve() / 'build/Debug'
        self.jobs.runtime_health(candidate)
        self.jobs.runtime_health(self.paths.runtime)
        if not (candidate / 'quakeray.exe').is_file() or not (candidate / 'id1/qray.pkz').is_file():
            raise EvidenceError('ENV_MISSING_BUILD', 'Build the isolated experiment candidate first')
        match = next((value for value in self.list_scenarios()['scenarios'] if value['scenario_id'] == scenario_id), None)
        if not match or match['missing']:
            raise EvidenceError('ENV_MISSING_SCENARIO', 'Approved baseline fixtures are required')
        arguments = {'experiment_id': experiment_id, 'scenario_id': scenario_id, 'idempotency_key': idempotency_key,
                     'baseline': str(self.paths.runtime), 'asset_source': str(self.paths.runtime),
                     'preset': match['preset'], 'save': match['save'], 'seconds': seconds,
                     'warmup': warmup_s, 'repeats': 1, 'stats_level': 0}
        arguments['order'] = order
        return self.jobs.start('suite', arguments, self.jobs.worker_command('suite', arguments, candidate), 600)

    def remove_experiment(self, experiment_id, confirm=False):
        return self.research.remove_experiment(experiment_id, confirm)
