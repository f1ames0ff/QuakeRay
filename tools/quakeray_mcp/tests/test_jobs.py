import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

from quakeray_mcp.errors import EvidenceError
from quakeray_mcp.jobs import JobManager, process_identity
from quakeray_mcp.windows_process import ContainedProcess


ROOT = Path(__file__).resolve().parents[3]
WORKER = Path(__file__).parent / "fixtures/fake_worker.py"
TEMP_ROOT = Path(os.environ.get("LOCALAPPDATA", tempfile.gettempdir())) / "Temp/opencode"
if not TEMP_ROOT.is_dir():
    TEMP_ROOT = Path(tempfile.gettempdir())


@unittest.skipUnless(os.name == "nt", "Windows containment tests")
class JobTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=TEMP_ROOT)
        self.directory = Path(self.temp.name)
        self.manager = JobManager(ROOT, self.directory / "jobs")

    def tearDown(self):
        self.manager.close()
        self.temp.cleanup()

    def launch(self, mode="sleep", seconds=0.2, key="test_request_key_01", timeout=5, file=None):
        command = [sys.executable, "-B", str(WORKER), mode, "--seconds", str(seconds)]
        if file:
            command += ["--file", str(file)]
        return self.manager.start("fake", {"idempotency_key": key, "mode": mode, "seconds": seconds},
                                  lambda directory, deadline: command, timeout)

    def finish(self, receipt):
        result = self.manager.get(receipt["job_id"], 6)
        self.assertIn(result["state"], {"succeeded", "failed", "cancelled", "interrupted"})
        return result

    def test_success_and_failure(self):
        self.assertEqual(self.finish(self.launch())["state"], "succeeded")
        self.assertEqual(self.finish(self.launch("fail", key="test_request_key_02"))["exit_code"], 7)

    def test_cooperative_cancel_before_forced_termination(self):
        receipt = self.manager.start("fake", {"idempotency_key": "cooperative_stop_01"},
            lambda directory, deadline: [sys.executable, "-B", str(WORKER), "cooperate", "--stop-file", str(directory / "stop.request")], 10)
        deadline = time.monotonic() + 3
        while self.manager.get(receipt["job_id"])["state"] == "accepted" and time.monotonic() < deadline:
            time.sleep(0.02)
        self.manager.cancel(receipt["job_id"], receipt["control_token"])
        result = self.finish(receipt)
        self.assertEqual(result["state"], "cancelled")
        self.assertEqual(result["stop_mode"], "cooperative")

    def test_idempotent_start_returns_same_job(self):
        receipt = self.launch(seconds=0.5)
        retry = self.launch(seconds=0.5)
        self.assertEqual(receipt["job_id"], retry["job_id"])
        self.assertIsNone(retry["control_token"])
        self.finish(receipt)

    def test_changed_arguments_conflict(self):
        receipt = self.launch(seconds=0.5)
        with self.assertRaises(EvidenceError) as error:
            self.launch(seconds=1)
        self.assertEqual(error.exception.code, "IDEMPOTENCY_CONFLICT")
        self.finish(receipt)

    def test_only_one_active_job_per_server(self):
        receipt = self.launch(seconds=0.5)
        with self.assertRaises(EvidenceError) as error:
            self.launch(key="test_request_key_02")
        self.assertEqual(error.exception.code, "MACHINE_BUSY")
        self.finish(receipt)

    def test_timeout_is_not_reset_by_polling(self):
        receipt = self.launch(seconds=30, timeout=0.4)
        self.manager.get(receipt["job_id"], 0.1)
        result = self.finish(receipt)
        self.assertEqual(result["error"]["code"], "RUN_TIMEOUT")

    def test_cancel_requires_capability(self):
        receipt = self.launch(seconds=30)
        with self.assertRaises(EvidenceError):
            self.manager.cancel(receipt["job_id"], "incorrect")
        self.manager.cancel(receipt["job_id"], receipt["control_token"])
        result = self.finish(receipt)
        self.assertEqual(result["state"], "cancelled")
        self.manager.cancel(receipt["job_id"], receipt["control_token"])

    def test_logs_and_store_do_not_expose_control_token(self):
        receipt = self.launch()
        self.finish(receipt)
        text = (self.manager.store.directory(receipt["job_id"]) / "job.json").read_text()
        self.assertNotIn(receipt["control_token"], text)
        self.assertNotIn("control_hash", self.manager.get(receipt["job_id"]))

    def test_containment_failure_never_executes_worker(self):
        marker = self.directory / "marker"
        called = []
        def reject(job, process):
            called.append(True)
            raise RuntimeError("Deliberate assignment failure")
        with self.assertRaises(EvidenceError) as error:
            ContainedProcess([sys.executable, "-B", str(WORKER), "marker", "--file", str(marker)],
                             ROOT, self.directory / "stdout", self.directory / "stderr", assign=reject)
        self.assertEqual(error.exception.code, "PROCESS_CONTAINMENT_FAILED")
        self.assertFalse(marker.exists())
        self.assertEqual(called, [True])

    def test_cancel_kills_grandchild(self):
        marker = self.directory / "pids.json"
        receipt = self.launch("child", seconds=30, file=marker)
        deadline = time.monotonic() + 5
        while not marker.is_file() and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertTrue(marker.is_file())
        pids = json.loads(marker.read_text())
        self.assertIsNotNone(process_identity(pids["child"]))
        self.manager.cancel(receipt["job_id"], receipt["control_token"])
        self.finish(receipt)
        deadline = time.monotonic() + 5
        while process_identity(pids["child"]) and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertIsNone(process_identity(pids["child"]))

    def test_owner_death_closes_job_and_descendants(self):
        marker = self.directory / "pids.json"
        script = (
            "import sys,time; from pathlib import Path; from quakeray_mcp.windows_process import ContainedProcess; "
            f"p=ContainedProcess([sys.executable,'-B',{str(WORKER)!r},'child','--file',{str(marker)!r}],"
            f"Path({str(ROOT)!r}),Path({str(self.directory / 'out')!r}),Path({str(self.directory / 'err')!r})); time.sleep(30)"
        )
        owner = subprocess.Popen([sys.executable, "-B", "-c", script])
        try:
            deadline = time.monotonic() + 5
            while not marker.is_file() and time.monotonic() < deadline:
                time.sleep(0.02)
            self.assertTrue(marker.is_file())
            pids = json.loads(marker.read_text())
            owner.kill()
            owner.wait(5)
            deadline = time.monotonic() + 5
            while (process_identity(pids["parent"]) or process_identity(pids["child"])) and time.monotonic() < deadline:
                time.sleep(0.02)
            self.assertIsNone(process_identity(pids["parent"]))
            self.assertIsNone(process_identity(pids["child"]))
        finally:
            if owner.poll() is None:
                owner.kill()
            owner.wait(5)

    def test_restart_marks_dead_owner_interrupted(self):
        receipt = self.launch()
        self.finish(receipt)
        record = self.manager.store.read(receipt["job_id"])
        record.update(state="running", server_pid=2147483647, server_creation="not-a-process")
        self.manager.store.write(receipt["job_id"], record)
        other = JobManager(ROOT, self.manager.store.root)
        try:
            self.assertEqual(other.get(receipt["job_id"])["state"], "interrupted")
        finally:
            other.close()

    def test_other_owner_cannot_cancel(self):
        receipt = self.launch(seconds=0.5)
        other = JobManager(ROOT, self.manager.store.root)
        try:
            with self.assertRaises(EvidenceError):
                other.cancel(receipt["job_id"], receipt["control_token"])
        finally:
            other.close()
        self.finish(receipt)

    def test_mutating_tools_are_opt_in_and_missing_runtime_is_typed(self):
        from quakeray_mcp.service import EvidenceService
        readonly = EvidenceService(ROOT, enable_jobs=False)
        self.assertNotIn("start_build", readonly.tool_names)
        enabled = EvidenceService(ROOT, enable_jobs=True, state_root=self.directory / "service")
        try:
            enabled.paths.runtime = self.directory / "missing-runtime"
            self.assertIn("start_build", enabled.tool_names)
            with self.assertRaises(EvidenceError) as error:
                enabled.start_benchmark("qr_fuma_start-balanced", "debug", "request_key_123456")
            self.assertEqual(error.exception.code, "ENV_MISSING_BUILD")
        finally:
            enabled.close()

    def test_runtime_health_rejects_failed_build(self):
        receipt = self.launch()
        self.finish(receipt)
        directory = self.manager.store.directory(receipt["job_id"])
        runtime = self.directory / "runtime"
        (directory / "request.json").write_text(json.dumps({"operation": "build", "runtime": str(runtime)}))
        record = self.manager.store.read(receipt["job_id"])
        record.update(operation="build", state="failed", recovery="runtime_and_submodule_review_required")
        self.manager.store.write(receipt["job_id"], record)
        with self.assertRaises(EvidenceError) as error:
            self.manager.runtime_health(runtime)
        self.assertEqual(error.exception.code, "RECOVERY_REQUIRED")

    def capture_fixture(self, preset="quality"):
        receipt = self.launch()
        self.finish(receipt)
        directory = self.manager.store.directory(receipt["job_id"])
        fixture_root = WORKER.parent
        path = directory / "example.frames.csv"
        path.write_text((fixture_root / "frames.csv").read_text())
        bench_text = (fixture_root / "benchmark.log").read_text()
        if preset == "quality":
            bench_text = bench_text.replace("rt_upscale_fsr31=3", "rt_upscale_fsr31=2")
        (directory / "example.bench.log").write_text(bench_text)
        (directory / "manifest.json").write_text(json.dumps({"ExecutableSha256": "unverified_test_value"}))
        (directory / "request.json").write_text(json.dumps({"preset": preset}))
        return receipt, directory, path

    def test_quality_budget_and_provenance_retained(self):
        receipt, directory, path = self.capture_fixture()
        ids = self.manager._collect_runs(receipt["job_id"], {"captures": [str(path)]})
        record = self.manager.list_runs()["runs"][0]
        self.assertEqual(record["run_id"], ids[0])
        self.assertAlmostEqual(record["metrics"]["budget_ms"], 1000 / 45)
        self.assertEqual(record["metrics"]["over_budget_percent"], 0)
        self.assertIn("effective_settings", record["provenance"])
        self.assertFalse(record["provenance_verified"])

    def test_partial_batch_never_published(self):
        receipt, directory, path = self.capture_fixture()
        bad = directory / "bad.frames.csv"
        bad.write_text("truncated")
        with self.assertRaises(EvidenceError):
            self.manager._collect_runs(receipt["job_id"], {"captures": [str(path), str(bad)]})
        self.assertEqual(self.manager.list_runs()["runs"], [])
        self.assertFalse((directory / "runs/batch.json").exists())

    def test_benchmark_association_mismatch_rejected(self):
        receipt, directory, path = self.capture_fixture()
        bench = directory / "example.bench.log"
        bench.write_text(bench.read_text().replace("demo=maps/start.bsp", "demo=maps/other.bsp"))
        with self.assertRaises(EvidenceError):
            self.manager._collect_runs(receipt["job_id"], {"captures": [str(path)]})

    def test_prune_preview_and_completed_evidence_protection(self):
        receipt = self.launch()
        self.finish(receipt)
        directory = self.manager.store.directory(receipt['job_id'])
        self.assertTrue(self.manager.prune(0, True)['dry_run'])
        self.assertTrue(directory.exists())
        record = self.manager.store.read(receipt['job_id'])
        record['run_ids'] = ['run_' + 'a' * 32]
        self.manager.store.write(receipt['job_id'], record)
        self.assertEqual(self.manager.prune(0, False)['jobs'], [])
        self.assertTrue(directory.exists())


if __name__ == "__main__":
    unittest.main()
