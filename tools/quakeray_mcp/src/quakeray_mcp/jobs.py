from datetime import datetime, timezone
import hashlib
import hmac
import json
import os
from pathlib import Path
import secrets
import subprocess
import threading
import shutil
import stat
import re
import time
import uuid

from .errors import EvidenceError
from .job_store import JobStore
from .parsers import load_analyzer, parse_stats
from .run_records import prepare_records
from .windows_process import ContainedProcess


TERMINAL = {"succeeded", "failed", "cancelled", "interrupted"}


def process_identity(pid):
    import win32api
    import win32process
    try:
        handle = win32api.OpenProcess(0x1000, False, pid)
        try:
            if win32process.GetExitCodeProcess(handle) != 259:
                return None
            return str(win32process.GetProcessTimes(handle)["CreationTime"])
        finally:
            handle.Close()
    except Exception as exc:
        code = getattr(exc, "winerror", exc.args[0] if exc.args else None)
        return "unavailable" if code == 5 else None


class JobManager:
    def __init__(self, root, state_root):
        self.root = Path(root).resolve()
        self.store = JobStore(state_root)
        self.instance = uuid.uuid4().hex
        self.server_pid = os.getpid()
        self.server_creation = process_identity(self.server_pid)
        self.active = {}
        self.lock = threading.RLock()
        self.closed = False
        with self.store.locked():
            for path in self.store.root.glob("job_*/job.json"):
                record = self.store.read(path.parent.name)
                identity = process_identity(record["server_pid"])
                if record["state"] not in TERMINAL and identity != "unavailable" and identity != record["server_creation"]:
                    record.update(state="interrupted", error={"code": "RECOVERY_REQUIRED", "message": "Owner exited; inspect retained job evidence"})
                    self.store.write(record["job_id"], record)

    def public(self, record):
        return {key: value for key, value in record.items() if key not in {"key_hash", "control_hash", "server_creation"}}

    def start(self, operation, arguments, command_factory, timeout):
        encoded = json.dumps({"operation": operation, "arguments": arguments}, sort_keys=True, allow_nan=False)
        args_hash = hashlib.sha256(encoded.encode()).hexdigest()
        key_hash = hashlib.sha256(arguments["idempotency_key"].encode()).hexdigest()
        with self.lock, self.store.locked():
            if self.closed:
                raise EvidenceError("SERVICE_CLOSED", "Job manager is shutting down")
            used = sum(path.stat().st_size for path in self.store.root.rglob('*') if path.is_file() and not path.is_symlink())
            quota = 32 * 1024 * 1024 * 1024
            reservations = sum(self.store.read(path.parent.name).get('reserved_bytes', 0) for path in self.store.root.glob('job_*/job.json')
                               if self.store.read(path.parent.name)['state'] not in TERMINAL)
            reserve = 16 * 1024 * 1024 * 1024 if arguments.get('baseline') else 8 * 1024 * 1024 * 1024
            if used + reservations + reserve > quota:
                raise EvidenceError('DISK_QUOTA', 'Job store reached its 32 GiB quota; inspect/prune retained jobs first')
            for path in self.store.root.glob("job_*/job.json"):
                old = self.store.read(path.parent.name)
                if old["key_hash"] == key_hash:
                    if old["arguments_sha256"] != args_hash:
                        raise EvidenceError("IDEMPOTENCY_CONFLICT", "This request key already identifies different arguments")
                    result = self.public(old)
                    result["control_token"] = None
                    return result
            if any(not entry["finished"].is_set() for entry in self.active.values()):
                raise EvidenceError("MACHINE_BUSY", "This server already has an active heavy job", retryable=True)
            identifier = "job_" + uuid.uuid4().hex
            token = secrets.token_urlsafe(32)
            now = datetime.now(timezone.utc)
            record = {"schema_version": 1, "job_id": identifier, "operation": operation, "state": "accepted",
                      "owner_instance": self.instance, "server_pid": self.server_pid, "server_creation": self.server_creation,
                      "key_hash": key_hash, "control_hash": hashlib.sha256(token.encode()).hexdigest(),
                      "arguments_sha256": args_hash, "created_utc": now.isoformat(), "deadline_seconds": timeout,
                      "reserved_bytes": reserve,
                      "exit_code": None, "error": None, "run_ids": [], "recovery": "not_required"}
            self.store.write(identifier, record)
            directory = self.store.directory(identifier)
            try:
                command = command_factory(directory, now.timestamp() + timeout)
            except Exception:
                record.update(state="failed", error={"code": "PREPARATION_FAILED", "message": "Worker request could not be prepared"})
                self.store.write(identifier, record)
                raise
            entry = {"cancel": threading.Event(), "finished": threading.Event(), "process": None}
            self.active[identifier] = entry
            thread = threading.Thread(target=self._run, args=(identifier, command, timeout, entry), daemon=True)
            entry["thread"] = thread
            thread.start()
            result = self.public(record)
            result["control_token"] = token
            return result

    def _run(self, identifier, command, timeout, entry):
        process = None
        started = time.monotonic()
        directory = self.store.directory(identifier)
        record = self.store.read(identifier)
        try:
            if entry["cancel"].is_set():
                record["state"] = "cancelled"
                return
            process = ContainedProcess(command, self.root, directory / "stdout.log", directory / "stderr.log")
            entry["process"] = process
            record.update(state="running", worker_pid=process.pid, worker_creation=process.creation_time)
            with self.store.locked():
                self.store.write(identifier, record)
            while True:
                exit_code = process.wait(100)
                if exit_code is not None:
                    record.update(exit_code=exit_code, state="succeeded" if exit_code == 0 else "failed")
                    break
                if entry["cancel"].is_set() or time.monotonic() - started >= timeout:
                    (directory / "stop.request").write_text("stop", encoding="ascii")
                    record["state"] = "cancelling"
                    with self.store.locked():
                        self.store.write(identifier, record)
                    exit_code = process.wait(2000)
                    forced = exit_code is None
                    if forced:
                        process.terminate()
                        exit_code = process.wait(5000)
                    record.update(exit_code=exit_code, state="cancelled" if entry["cancel"].is_set() else "failed")
                    record["stop_mode"] = "forced" if forced else "cooperative"
                    record["error"] = {"code": "CANCELLED" if entry["cancel"].is_set() else "RUN_TIMEOUT",
                                       "message": "Owned process tree terminated; in-memory capture may be unavailable"}
                    break
            if record["state"] == "failed" and record["error"] is None:
                tail = self._tail(directory / "stderr.log")
                known = next((code for code in ("MACHINE_BUSY", "BUILD_FAILED", "RUN_TIMEOUT", "PERMISSION_DENIED", "DISK_QUOTA") if code in tail), "PROCESS_FAILED")
                record["error"] = {"code": known, "message": tail or "Worker exited unsuccessfully"}
            if record["operation"] == "build" and record["state"] != "succeeded":
                record["recovery"] = "runtime_and_submodule_review_required"
            if record["state"] == "succeeded" and record["operation"] != "fake":
                result_path = directory / "result.json"
                if not result_path.is_file():
                    raise EvidenceError("RESULT_MISSING", "Worker exited without a completion record")
                result = json.loads(result_path.read_text(encoding="utf-8-sig"))
                record["result"] = result
                if record["operation"] != "build":
                    record["run_ids"] = self._collect_runs(identifier, result)
        except EvidenceError as exc:
            record.update(state="failed", error=exc.payload())
        except Exception:
            record.update(state="failed", error={"code": "INTERNAL", "message": "Job supervision failed; retained evidence needs review"})
        finally:
            if process:
                process.close()
            if record["operation"] == "build" and record["state"] != "succeeded":
                record["recovery"] = "runtime_and_submodule_review_required"
            with self.store.locked():
                record["finished_utc"] = datetime.now(timezone.utc).isoformat()
                self.store.write(identifier, record)
            entry["finished"].set()

    def _tail(self, path):
        if not path.is_file():
            return ""
        with path.open("rb") as stream:
            stream.seek(max(0, path.stat().st_size - 8192))
            return stream.read(8192).decode("utf-8", errors="replace")

    def _collect_runs(self, identifier, result):
        directory = self.store.directory(identifier)
        analyzer = load_analyzer(self.root)
        records = prepare_records(directory, self.store.root, identifier, result, analyzer)
        identifiers = [record["run_id"] for record in records]
        runs = directory / "runs"
        runs.mkdir(exist_ok=True)
        temporary = runs / (uuid.uuid4().hex + ".tmp")
        with self.store.locked():
            temporary.write_text(json.dumps(records, allow_nan=False, indent=2), encoding="utf-8")
            os.replace(temporary, runs / "batch.json")
            for record in records:
                with (self.store.root / "index.jsonl").open("a", encoding="utf-8") as stream:
                    stream.write(json.dumps({"run_id": record["run_id"], "job_id": identifier, "batch": str((runs / "batch.json").relative_to(self.store.root))}) + "\n")
        return identifiers

    def list_runs(self, last_n=20):
        paths = sorted([*self.store.root.glob("job_*/runs/batch.json"), *self.store.root.glob("job_*/runs/run_*.json")],
                       key=lambda path: path.stat().st_mtime, reverse=True)
        records = []
        for path in paths:
            if not path.resolve().is_relative_to(self.store.root) or path.stat().st_size > 4 * 1024 * 1024:
                raise EvidenceError("RECOVERY_REQUIRED", "Run catalog entry needs manual review")
            value = json.loads(path.read_text(encoding="utf-8"))
            records.extend(value if isinstance(value, list) else [value])
            if len(records) >= last_n:
                break
        return {"runs": records[:last_n], "acceptance": "not_checked"}

    def find_run(self, identifier):
        if not isinstance(identifier, str) or len(identifier) != 36 or not identifier.startswith('run_'):
            raise EvidenceError('NOT_FOUND', 'Unknown run ID')
        for path in self.store.root.glob('job_*/runs/batch.json'):
            if path.stat().st_size > 4 * 1024 * 1024:
                raise EvidenceError('RECOVERY_REQUIRED', 'Run batch exceeds its bound')
            for record in json.loads(path.read_text(encoding='utf-8')):
                if record['run_id'] == identifier:
                    return record
        raise EvidenceError('NOT_FOUND', 'Run is not retained in this catalog')

    def verify_run(self, identifier):
        record = self.find_run(identifier)
        path = (self.store.root / record['artifact_path']).resolve()
        if not path.is_relative_to(self.store.root):
            raise EvidenceError('PERMISSION_DENIED', 'Run evidence escapes the store')
        from .run_records import bounded_bytes
        raw = bounded_bytes(path)
        if hashlib.sha256(raw).hexdigest() != record['sha256']:
            raise EvidenceError('EVIDENCE_CHANGED', 'Retained capture no longer matches its hash')
        directory = self.store.directory(record['job_id'])
        refreshed = prepare_records(directory, self.store.root, record['job_id'], {'captures': [str(path)]}, load_analyzer(self.root))[0]
        return {'run_id': identifier, 'capture_integrity': 'verified', 'provenance_verified': refreshed['provenance_verified'],
                'build_config': record.get('build_config', 'unknown'), 'visual': 'not_checked',
                'reason': 'Integrity alone does not establish binary/control/asset compatibility'}

    def promote_baseline(self, identifier, name, confirm=False):
        if not re.fullmatch(r'[A-Za-z0-9_-]{1,64}', name):
            raise EvidenceError('INVALID_ARGUMENT', 'Safe baseline name required')
        verification = self.verify_run(identifier)
        if not verification['provenance_verified']:
            raise EvidenceError('INCOMPARABLE', 'Baseline provenance must be verified first')
        if not confirm:
            return {'confirmation_required': True, 'name': name, 'run_id': identifier}
        with self.store.locked():
            folder = self.store.root.parent / 'baselines'
            folder.mkdir(exist_ok=True)
            path = folder / (name + '.json')
            if path.exists():
                raise EvidenceError('BASELINE_EXISTS', 'Existing baseline is preserved; select a new name')
            value = {'name': name, 'run_id': identifier, 'capture_sha256': self.find_run(identifier)['sha256'],
                     'verification': verification}
            temporary = folder / (uuid.uuid4().hex + '.tmp')
            temporary.write_text(json.dumps(value, allow_nan=False), encoding='utf-8')
            os.replace(temporary, path)
            return value

    def runtime_health(self, runtime):
        records = []
        for path in self.store.root.glob("job_*/request.json"):
            if path.stat().st_size > 1024 * 1024:
                raise EvidenceError("RECOVERY_REQUIRED", "Worker request is malformed")
            request = json.loads(path.read_text(encoding="utf-8"))
            if request.get("operation") == "build" and Path(request["runtime"]).resolve() == Path(runtime).resolve():
                records.append(self.store.read(path.parent.name))
        if records:
            newest = max(records, key=lambda record: record["created_utc"])
            if newest["state"] != "succeeded":
                raise EvidenceError("RECOVERY_REQUIRED", "Latest build is incomplete; inspect/rebuild the runtime before launching it")

    def prune(self, keep_last=20, dry_run=True):
        with self.store.locked():
            records = [self.store.read(path.parent.name) for path in self.store.root.glob('job_*/job.json')]
            records.sort(key=lambda value: value['created_utc'], reverse=True)
            removable = [record for record in records[keep_last:] if record['state'] in TERMINAL and not record.get('run_ids')]
            plan = [{'job_id': record['job_id'], 'path': str(self.store.directory(record['job_id']))} for record in removable]
            if not dry_run:
                for record in removable:
                    directory = self.store.directory(record['job_id'])
                    if any(path.is_symlink() or getattr(path.lstat(), 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT
                           for path in directory.rglob('*')):
                        raise EvidenceError('PERMISSION_DENIED', 'Refusing cleanup of a job with redirected paths')
                    shutil.rmtree(directory)
            return {'dry_run': dry_run, 'jobs': plan, 'protected_completed_evidence': True}

    def get(self, identifier, wait_seconds=0):
        entry = self.active.get(identifier)
        if entry:
            entry["finished"].wait(wait_seconds)
        record = self.public(self.store.read(identifier))
        directory = self.store.directory(identifier)
        record["stdout_tail"] = self._tail(directory / "stdout.log")
        record["stderr_tail"] = self._tail(directory / "stderr.log")
        record["untrusted_data"] = True
        return record

    def cancel(self, identifier, token):
        record = self.store.read(identifier)
        if record["owner_instance"] != self.instance or not isinstance(token, str) or not hmac.compare_digest(
                hashlib.sha256(token.encode()).hexdigest(), record["control_hash"]):
            raise EvidenceError("PERMISSION_DENIED", "Creator control capability is required to cancel this job")
        if record["state"] not in TERMINAL:
            self.active[identifier]["cancel"].set()
        return self.get(identifier)

    def close(self):
        with self.lock:
            self.closed = True
            entries = list(self.active.values())
            for entry in entries:
                entry["cancel"].set()
        for entry in entries:
            entry["finished"].wait(6)

    def worker_command(self, operation, arguments, runtime):
        def prepare(directory, deadline):
            spec = {key: value for key, value in arguments.items() if key != "idempotency_key"}
            spec.update(operation=operation, repo_root=str(self.root), runtime=str(runtime),
                        deadline_utc=datetime.fromtimestamp(deadline, timezone.utc).isoformat())
            path = directory / "request.json"
            path.write_text(json.dumps(spec, allow_nan=False), encoding="utf-8")
            return ["powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File",
                    str(self.root / "tools/quakeray_mcp/worker.ps1"), "-Spec", str(path)]
        return prepare
