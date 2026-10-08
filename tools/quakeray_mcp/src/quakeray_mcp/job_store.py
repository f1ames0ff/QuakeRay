from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import uuid

from .errors import EvidenceError


class JobStore:
    def __init__(self, root):
        self.root = Path(root).resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.lock_name = "Local\\QuakeRayMCPStore_" + hashlib.sha256(str(self.root).encode()).hexdigest()[:24]

    @contextmanager
    def locked(self):
        import win32event
        import win32api
        handle = win32event.CreateMutex(None, False, self.lock_name)
        owned = False
        try:
            result = win32event.WaitForSingleObject(handle, 5000)
            if result not in (win32event.WAIT_OBJECT_0, win32event.WAIT_ABANDONED):
                raise EvidenceError("LOCK_TIMEOUT", "Evidence store is occupied", retryable=True)
            owned = True
            yield
        finally:
            if owned:
                win32event.ReleaseMutex(handle)
            win32api.CloseHandle(handle)

    def directory(self, identifier):
        if not isinstance(identifier, str) or not identifier.startswith("job_") or len(identifier) != 36:
            raise EvidenceError("NOT_FOUND", "Unknown job ID")
        try:
            int(identifier[4:], 16)
        except ValueError as exc:
            raise EvidenceError("NOT_FOUND", "Unknown job ID") from exc
        path = (self.root / identifier).resolve()
        if not path.is_relative_to(self.root):
            raise EvidenceError("PERMISSION_DENIED", "Job path escapes the store")
        return path

    def write(self, identifier, record):
        directory = self.directory(identifier)
        directory.mkdir(exist_ok=True)
        temporary = directory / (uuid.uuid4().hex + ".tmp")
        with temporary.open("w", encoding="utf-8") as stream:
            json.dump(record, stream, allow_nan=False, indent=2)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, directory / "job.json")

    def read(self, identifier):
        path = self.directory(identifier) / "job.json"
        try:
            if path.stat().st_size > 1024 * 1024:
                raise EvidenceError("RECOVERY_REQUIRED", "Job manifest exceeds its size bound")
            record = json.loads(path.read_text(encoding="utf-8"))
            if record["job_id"] != identifier or record["schema_version"] != 1:
                raise ValueError("Invalid job identity")
            return record
        except FileNotFoundError as exc:
            raise EvidenceError("NOT_FOUND", "Job is not in the store") from exc
        except (KeyError, ValueError, OSError) as exc:
            raise EvidenceError("RECOVERY_REQUIRED", "Job manifest needs manual review") from exc
