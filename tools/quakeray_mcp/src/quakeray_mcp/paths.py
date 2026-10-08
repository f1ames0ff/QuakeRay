import hashlib
import os
from pathlib import Path

from .errors import EvidenceError


MAX_FILE_BYTES = 64 * 1024 * 1024


class ApprovedPaths:
    def __init__(self, root, import_roots=()):
        self.root = Path(root).resolve(strict=True)
        self.import_roots = tuple(Path(p).resolve() for p in import_roots)
        self.runtime = (self.root / os.environ.get("QUAKERAY_BUILD", "build/Debug")).resolve()
        self.roots = (self.runtime, *self.import_roots)

    def resolve_import(self, value, extensions=None):
        if not isinstance(value, str) or not value or len(value) > 2048:
            raise EvidenceError("INVALID_ARGUMENT", "An import path is required")
        path = Path(value)
        path = (self.root / path).resolve() if not path.is_absolute() else path.resolve()
        if not any(path.is_relative_to(root) for root in self.roots):
            raise EvidenceError("PERMISSION_DENIED", "Import path is outside approved roots",
                                "Configure QUAKERAY_IMPORT_ROOTS explicitly before importing evidence")
        if not path.is_file():
            raise EvidenceError("NOT_FOUND", "Evidence file does not exist")
        if path.suffix.lower() not in (extensions or {".csv", ".dump", ".log", ".json"}):
            raise EvidenceError("INVALID_ARGUMENT", "Unsupported evidence file extension")
        return path

    def read(self, path):
        try:
            with path.open("rb") as stream:
                before = os.fstat(stream.fileno())
                raw = stream.read(MAX_FILE_BYTES + 1)
                after = os.fstat(stream.fileno())
            if len(raw) > MAX_FILE_BYTES:
                raise EvidenceError("FILE_TOO_LARGE", "Evidence exceeds the 64 MiB import limit")
            if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                raise EvidenceError("EVIDENCE_CHANGED", "Evidence changed while being read", retryable=True)
            encoding = "utf-16" if raw.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
            text = raw.decode(encoding).replace("\r\n", "\n").replace("\r", "\n")
            return text, hashlib.sha256(raw).hexdigest(), len(raw)
        except (OSError, UnicodeError) as exc:
            raise EvidenceError("READ_FAILED", "Evidence cannot be read as UTF-8 or BOM-marked UTF-16") from exc
