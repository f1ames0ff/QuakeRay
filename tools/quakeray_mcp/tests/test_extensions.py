import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from quakeray_mcp.errors import EvidenceError
from quakeray_mcp.job_store import JobStore
from quakeray_mcp.research import ResearchStore


@unittest.skipUnless(os.name == "nt", "Windows shared store")
class ResearchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name) / "repo"
        self.root.mkdir()
        subprocess.run(["git", "init", str(self.root)], check=True, capture_output=True)
        (self.root / "fixture.txt").write_text("fixture")
        self.git("add", "fixture.txt")
        self.git("-c", "user.name=Test", "-c", "user.email=test@example.invalid", "-c", "commit.gpgSign=false", "commit", "-m", "fixture")
        self.store = JobStore(Path(self.temp.name) / "state/jobs")
        self.research = ResearchStore(self.root, self.store)

    def tearDown(self):
        for path in self.research.directory.glob("experiment_*.json"):
            record = self.research.read(path.stem)
            directory = Path(record["worktree"])
            if directory.is_dir():
                note = directory / "note.txt"
                if note.exists():
                    note.unlink()
                self.research.remove_experiment(path.stem, True)
        self.temp.cleanup()

    def git(self, *args):
        return subprocess.run(["git", "-C", str(self.root), *args], capture_output=True, check=True,
                              encoding="utf-8").stdout.strip()

    def test_worktree_isolated_dirty_removal_refused(self):
        before = self.git("status", "--porcelain")
        record = self.research.create_experiment(self.git("rev-parse", "HEAD"), "test")
        path = Path(record["worktree"])
        self.assertEqual((path / "fixture.txt").read_text(), "fixture")
        (path / "note.txt").write_text("preserve me")
        with self.assertRaises(EvidenceError):
            self.research.remove_experiment(record["experiment_id"], True)
        self.assertTrue((path / "note.txt").exists())
        self.assertEqual(self.git("status", "--porcelain"), before)

    def test_preview_does_not_remove(self):
        record = self.research.create_experiment(self.git("rev-parse", "HEAD"), "test")
        self.assertTrue(self.research.remove_experiment(record["experiment_id"])["dry_run"])
        self.assertTrue(Path(record["worktree"]).is_dir())

    def test_unknown_run_cannot_become_finding(self):
        with self.assertRaises(EvidenceError):
            self.research.record_finding("hypothesis", "particles", ["run_" + "a" * 32], "accepted")

    def test_finding_search_preserves_interpretation_label(self):
        folder = self.store.root / ("job_" + "a" * 32) / "runs"
        folder.mkdir(parents=True)
        identifier = "run_" + "b" * 32
        (folder / "batch.json").write_text(json.dumps([{"run_id": identifier}]))
        self.research.record_finding("Particles hypothesis", "particles", [identifier], "inconclusive")
        result = self.research.search("particles", "hypothesis")
        self.assertEqual(len(result["findings"]), 1)
        self.assertFalse(result["findings"][0]["verified"])
        self.assertEqual(result["findings"][0]["evidence_kind"], "operator_interpretation")

    def test_arbitrary_git_argument_rejected(self):
        with self.assertRaises(EvidenceError):
            self.research.create_experiment("--help", "escape")
