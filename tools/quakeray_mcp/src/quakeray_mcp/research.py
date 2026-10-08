from datetime import datetime, timezone
import json
from pathlib import Path
import re
import subprocess
import uuid

from .errors import EvidenceError


class ResearchStore:
    def __init__(self, root, store):
        self.root = Path(root).resolve()
        self.store = store
        self.directory = store.root.parent / "research"
        self.directory.mkdir(exist_ok=True)

    def write(self, identifier, record):
        temporary = self.directory / (uuid.uuid4().hex + ".tmp")
        temporary.write_text(json.dumps(record, indent=2, allow_nan=False), encoding="utf-8")
        temporary.replace(self.directory / (identifier + ".json"))

    def read(self, identifier):
        if not re.fullmatch(r"(?:finding|experiment)_[0-9a-f]{32}", identifier):
            raise EvidenceError("NOT_FOUND", "Unknown research ID")
        path = self.directory / (identifier + ".json")
        if not path.is_file() or path.stat().st_size > 1024 * 1024:
            raise EvidenceError("NOT_FOUND", "Research record is unavailable")
        return json.loads(path.read_text(encoding="utf-8"))

    def record_finding(self, hypothesis, subsystem, run_ids, decision):
        if not hypothesis or len(hypothesis) > 4000 or len(subsystem) > 64 or len(run_ids) > 20:
            raise EvidenceError("INVALID_ARGUMENT", "Finding fields exceed their bounds")
        known = set()
        for batch in self.store.root.glob("job_*/runs/batch.json"):
            for record in json.loads(batch.read_text(encoding="utf-8")):
                known.add(record["run_id"])
        if not run_ids or any(identifier not in known for identifier in run_ids):
            raise EvidenceError("NOT_FOUND", "Findings must reference retained run IDs")
        identifier = "finding_" + uuid.uuid4().hex
        record = {"finding_id": identifier, "hypothesis": hypothesis, "subsystem": subsystem,
                  "run_ids": run_ids, "decision": decision, "created_utc": datetime.now(timezone.utc).isoformat(),
                  "evidence_kind": "operator_interpretation", "verified": False}
        with self.store.locked():
            self.write(identifier, record)
        return record

    def search(self, subsystem="", query="", limit=20):
        records = []
        for path in sorted(self.directory.glob("finding_*.json"), reverse=True):
            record = self.read(path.stem)
            if subsystem and record["subsystem"] != subsystem:
                continue
            if query and query.casefold() not in record["hypothesis"].casefold():
                continue
            records.append(record)
            if len(records) >= limit:
                break
        return {"findings": records, "untrusted_data": True}

    def git(self, *arguments):
        result = subprocess.run(["git", "-C", str(self.root), *arguments], capture_output=True,
                                encoding="utf-8", errors="replace", timeout=60)
        if result.returncode:
            raise EvidenceError("GIT_FAILED", result.stderr[-2000:])
        return result.stdout.strip()

    def create_experiment(self, base_commit, name):
        if not re.fullmatch(r"[0-9a-fA-F]{7,40}", base_commit) or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", name):
            raise EvidenceError("INVALID_ARGUMENT", "Explicit commit SHA and safe experiment name required")
        revision = self.git("rev-parse", "--verify", base_commit + "^{commit}")
        identifier = "experiment_" + uuid.uuid4().hex
        path = self.directory / identifier
        self.git("worktree", "add", "--detach", str(path), revision)
        record = {"experiment_id": identifier, "name": name, "base_commit": revision, "worktree": str(path),
                  "state": "created", "dependency_status": "initialization_required", "run_ids": [],
                  "decision": "not_checked"}
        with self.store.locked():
            self.write(identifier, record)
        return record

    def suite_plan(self, identifier, scenarios):
        record = self.read(identifier)
        if record['dependency_status'] != 'local_pinned_checkouts_initialized':
            raise EvidenceError('ENV_MISSING_DEPENDENCIES', 'Prepare pinned experiment dependencies first')
        if not 1 <= len(scenarios) <= 2:
            raise EvidenceError('INVALID_ARGUMENT', 'A suite is limited to a primary and guard scenario')
        return {'experiment_id': identifier, 'source_root': record['worktree'], 'scenarios': scenarios,
                'build_config': 'Debug', 'build_once_per_source': True, 'one_scenario_per_job': True,
                'maximum_runtime_seconds_per_job': 300, 'acceptance': 'not_checked'}

    def finish_experiment(self, identifier, decision, run_ids):
        record = self.read(identifier)
        known = set()
        for batch in self.store.root.glob('job_*/runs/batch.json'):
            known.update(value['run_id'] for value in json.loads(batch.read_text(encoding='utf-8')))
        if any(value not in known for value in run_ids):
            raise EvidenceError('NOT_FOUND', 'Experiment decisions must reference retained run IDs')
        record.update(state="recorded", decision=decision, run_ids=run_ids)
        with self.store.locked():
            self.write(identifier, record)
        return record

    def prepare_experiment(self, identifier):
        record = self.read(identifier)
        path = Path(record['worktree']).resolve()
        if path != (self.directory / identifier).resolve():
            raise EvidenceError('PERMISSION_DENIED', 'Experiment ownership does not match')
        arguments = ['git', '-C', str(path), '-c', 'protocol.file.allow=always']
        for name in ('nvrhi', 'openal-soft'):
            local = self.root / 'third_party' / name
            if not (local / '.git').exists():
                raise EvidenceError('ENV_MISSING_DEPENDENCIES', 'Initialize the approved source submodules first')
            arguments += ['-c', f'submodule.third_party/{name}.url={local}']
        result = subprocess.run([*arguments, 'submodule', 'update', '--init', '--recursive', '--no-fetch'],
                                capture_output=True, encoding='utf-8', errors='replace', timeout=120)
        if result.returncode:
            raise EvidenceError('DEPENDENCIES_UNAVAILABLE', result.stderr[-2000:], 'No implicit remote fetch is performed')
        record['dependency_status'] = 'local_pinned_checkouts_initialized'
        with self.store.locked():
            self.write(identifier, record)
        return record

    def remove_experiment(self, identifier, confirm=False):
        record = self.read(identifier)
        path = Path(record["worktree"]).resolve()
        if path != (self.directory / identifier).resolve():
            raise EvidenceError("PERMISSION_DENIED", "Experiment ownership does not match")
        status = subprocess.run(["git", "-C", str(path), "status", "--porcelain", "--untracked-files=all"],
                                capture_output=True, encoding="utf-8", timeout=10, check=True).stdout
        if status:
            raise EvidenceError("DIRTY_WORKTREE", "Preserve experiment changes before removal; force removal is unavailable")
        if not confirm:
            return {"dry_run": True, "worktree": str(path), "decision": "confirmation_required"}
        self.git("worktree", "remove", str(path))
        record["state"] = "removed"
        with self.store.locked():
            self.write(identifier, record)
        return record
