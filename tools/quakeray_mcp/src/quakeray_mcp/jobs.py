from datetime import datetime, timezone
import hashlib
import hmac
import json
import os
from pathlib import Path
import secrets
import subprocess
import threading
import time
import uuid

from .errors import EvidenceError
from .job_store import JobStore
from .parsers import load_analyzer, parse_stats
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
        identifiers = []
        for value in result.get("captures", []):
            path = Path(value).resolve()
            if not path.is_relative_to(directory):
                raise EvidenceError("PERMISSION_DENIED", "Worker result refers to external evidence")
            raw = path.read_bytes()
            if len(raw) > 64 * 1024 * 1024:
                raise EvidenceError("FILE_TOO_LARGE", "Worker capture exceeds the import bound")
            capture = parse_stats(raw.decode("utf-8-sig").replace("\r\n", "\n"), analyzer, 1000 / 60)
            run_id = "run_" + uuid.uuid4().hex
            record = {"schema_version": 1, "run_id": run_id, "job_id": identifier,
                      "artifact_path": str(path.relative_to(self.store.root)), "sha256": hashlib.sha256(raw).hexdigest(),
                      "format": capture.format, "aggregation_kind": capture.aggregation_kind,
                      "metrics": capture.metrics, "validity": "captured_not_accepted",
                      "provenance_verified": False, "visual": "not_checked", "build_config": "unknown"}
            runs = directory / "runs"
            runs.mkdir(exist_ok=True)
            target = runs / (run_id + ".json")
            target.write_text(json.dumps(record, allow_nan=False, indent=2), encoding="utf-8")
            with self.store.locked():
                with (self.store.root / "index.jsonl").open("a", encoding="utf-8") as stream:
                    stream.write(json.dumps({"run_id": run_id, "job_id": identifier, "record": str(target.relative_to(self.store.root))}) + "\n")
            identifiers.append(run_id)
        if not identifiers:
            raise EvidenceError("CAPTURE_MISSING", "No completed capture was validated")
        return identifiers

    def list_runs(self, last_n=20):
        paths = sorted(self.store.root.glob("job_*/runs/run_*.json"), key=lambda path: path.stat().st_mtime, reverse=True)
        records = []
        for path in paths[:last_n]:
            if not path.resolve().is_relative_to(self.store.root) or path.stat().st_size > 1024 * 1024:
                raise EvidenceError("RECOVERY_REQUIRED", "Run catalog entry needs manual review")
            records.append(json.loads(path.read_text(encoding="utf-8")))
        return {"runs": records, "acceptance": "not_checked"}

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
