import copy
import csv
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from quakeray_mcp.compare import compare_records, IDENTITY_FIELDS
from quakeray_mcp.errors import EvidenceError
from quakeray_mcp.parsers import load_analyzer, metric_summaries, parse_benchmarks, parse_stats
from quakeray_mcp.paths import ApprovedPaths
from quakeray_mcp.service import EvidenceService


ROOT = Path(__file__).resolve().parents[3]
FIXTURES = Path(__file__).parent / "fixtures"


def fixture(name):
    return (FIXTURES / name).read_text(encoding="utf-8")


class ParserTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.analyzer = load_analyzer(ROOT)

    def test_frame_analyzer_parity(self):
        text = fixture("frames.csv")
        capture = parse_stats(text, self.analyzer, 1000 / 60)
        expected = self.analyzer.summarize(text, 1000 / 60)
        self.assertEqual(capture.metadata["summary"], expected)
        self.assertEqual(capture.metrics["fps"], 50)
        self.assertEqual(capture.metrics["p99_ms"], 20)

    def test_unknown_numeric_column(self):
        text = fixture("frames.csv").replace("ui_only\n", "ui_only,new_value\n")
        lines = text.splitlines()
        text = "\n".join(lines[:2] + [line + ",9" for line in lines[2:]])
        capture = parse_stats(text, self.analyzer, 20)
        self.assertIn("new_value", capture.columns)

    def test_invalid_frame_context(self):
        for text in (fixture("frames.csv").replace(",1,0,4,", ",0,0,4,"),
                     fixture("frames.csv").replace(",1,0,4,", ",1,1,4,"),
                     fixture("frames.csv").replace(",1,0,4,", ",1,0,3,"),
                     fixture("frames.csv").replace(",1,0,4,", ",0.9,0,4,")):
            with self.subTest(text=text), self.assertRaises(EvidenceError):
                parse_stats(text, self.analyzer, 20)

    def test_dropped_and_truncated(self):
        for text in (fixture("frames.csv").replace("dropped=0", "dropped=1"),
                     "\n".join(fixture("frames.csv").splitlines()[:-1])):
            with self.assertRaises(EvidenceError):
                parse_stats(text, self.analyzer, 20)

    def test_duplicate_columns_and_nonfinite(self):
        for text in (fixture("frames.csv").replace("host_interval_ms", "interval_ms"),
                     fixture("frames.csv").replace(",12,17,", ",nan,17,"),
                     fixture("frames.csv").replace(",12,17,", ",Infinity,17,")):
            with self.assertRaises(EvidenceError):
                parse_stats(text, self.analyzer, 20)

    def test_bad_clock_and_frozen_simulation(self):
        for text in (fixture("frames.csv").replace("1,30,20", "1,29,20"),
                     fixture("frames.csv").replace("1.08", "1.00")):
            with self.assertRaises(EvidenceError):
                parse_stats(text, self.analyzer, 20)

    def test_no_gpu_data_is_unavailable(self):
        text = fixture("frames.csv").replace(",4,1,12,", ",4,0,12,")
        capture = parse_stats(text, self.analyzer, 20)
        self.assertNotIn("gpu.frame_ms", capture.metrics)
        gpu = next(item for item in metric_summaries(capture, self.analyzer) if item["name"] == "gpu.frame_ms")
        self.assertIsNone(gpu["mean"])
        self.assertEqual(gpu["sample_count"], 0)

    def test_partial_interval_is_excluded_not_unavailable(self):
        capture = parse_stats(fixture("frames.csv"), self.analyzer, 20)
        value = next(item for item in metric_summaries(capture, self.analyzer) if item["name"] == "interval_ms")
        self.assertEqual(value["excluded_count"], 1)
        self.assertEqual(value["unavailable_count"], 0)
        self.assertEqual(value["sample_count"], 4)

    def test_window_blank_is_not_zero(self):
        capture = parse_stats(fixture("window.dump"), self.analyzer, 20)
        self.assertEqual(capture.aggregation_kind, "reporting_window")
        self.assertIsNone(capture.rows[1]["gpu.frame_ms"])
        self.assertEqual(capture.rows[0]["gpu.primary_ms"], 0)
        self.assertIn("new_counter", capture.columns)
        values = {item["name"]: item for item in metric_summaries(capture, self.analyzer)}
        self.assertEqual(values["cpu.frame_ms"]["aggregation_kind"], "window_maximum")
        self.assertEqual(values["gpu.frame_ms"]["aggregation_kind"], "completed_gpu_snapshot")

    def test_window_metadata_must_match(self):
        for text in (fixture("window.dump").replace("samples 2", "samples 3"),
                     fixture("window.dump").replace("duration 0.40", "duration 0.60")):
            with self.assertRaises(EvidenceError):
                parse_stats(text, self.analyzer, 20)

    def test_old_and_current_window_widths(self):
        for slot_count, expected in ((24, 110), (38, 124)):
            columns = ["t", "fps", "gpu.frame_ms"] + [f"gpu.pass_{i}_ms" for i in range(17)]
            columns += ["cpu.frame_ms", "cpu.main_ms", "cpu.wait_ms"]
            columns += [f"cpu.slot_{i}_ms" for i in range(slot_count)] + ["cpu.qrDrawFrame_avg_ms"]
            columns += [f"cpu.draw.pass_{i}_{kind}_ms" for i in range(19) for kind in ("avg", "max")]
            columns += [f"clust_{i}" for i in range(13)] + [f"rays_{i}" for i in range(6)] + [f"calls_{i}" for i in range(5)]
            stream = io.StringIO()
            stream.write("# rt_stats_dump_start 2026-10-08 10:00:00 panels 7 interval 0.200 samples 1 duration 0.20\n")
            writer = csv.writer(stream)
            writer.writerow(columns)
            writer.writerow([0.2] + [0] * (len(columns) - 1))
            self.assertEqual(len(parse_stats(stream.getvalue(), self.analyzer, 20).columns), expected)

    def test_snapshot_keeps_aggregation_and_backend_averages(self):
        capture = parse_stats(fixture("snapshot.log"), self.analyzer, 20)
        self.assertEqual(capture.format, "snapshot")
        self.assertEqual(capture.rows[0]["cpu.draw.RHI_setup_avg_ms"], 2.1)
        self.assertEqual(capture.rows[0]["cpu.slot.ents_ms"], 7)
        self.assertTrue(all(item["p95"] is None for item in metric_summaries(capture, self.analyzer)))

    def test_multiple_snapshots_select_latest_explicitly(self):
        text = fixture("snapshot.log") + "\n" + fixture("snapshot.log").replace("18.00", "20.00")
        capture = parse_stats(text, self.analyzer, 20)
        self.assertEqual(capture.metadata["snapshots_in_file"], 2)
        self.assertEqual(capture.metadata["selected_snapshot"], 1)
        self.assertEqual(capture.rows[0]["cpu.frame_ms"], 20)

    def test_snapshot_unavailable_gpu_total_is_not_zero(self):
        text = fixture("snapshot.log").replace("gpu.pass primary 0.00", "gpu.pass timings unavailable, the GPU timer queries did not run")
        capture = parse_stats(text, self.analyzer, 20)
        self.assertIsNone(capture.rows[0]["gpu.frame_ms"])

    def test_benchmark_settings_and_association(self):
        block = parse_benchmarks(fixture("benchmark.log"))[0]
        self.assertEqual(block["settings"]["vid"], "3840x2160@60")
        self.assertEqual(block["frame_samples"]["file"], "frames.csv")
        self.assertTrue(block["complete"])
        self.assertEqual(block["acceptance"], "not_checked")

    def test_interrupted_and_legacy_benchmarks_not_accepted(self):
        for text in (fixture("benchmark.log").replace("interrupted=0", "interrupted=1"),
                     "\n".join(fixture("benchmark.log").splitlines()[:-1])):
            self.assertFalse(parse_benchmarks(text)[0]["complete"])

    def test_fractional_and_negative_benchmark_counts_rejected(self):
        for text in (fixture("benchmark.log").replace("frames=5", "frames=5.1"),
                     fixture("benchmark.log").replace("samples=5", "samples=-1")):
            with self.assertRaises(EvidenceError):
                parse_benchmarks(text)


class ServiceTests(unittest.TestCase):
    def setUp(self):
        self.service = EvidenceService(ROOT, [FIXTURES])

    def test_path_outside_roots(self):
        with self.assertRaises(EvidenceError) as raised:
            self.service.read_stats({"import_path": "AGENTS.md"})
        self.assertEqual(raised.exception.code, "PERMISSION_DENIED")

    def test_path_traversal(self):
        with self.assertRaises(EvidenceError):
            self.service.read_stats({"import_path": str(FIXTURES / "../test_core.py")})

    def test_symlink_escape(self):
        with tempfile.TemporaryDirectory() as temp:
            approved = Path(temp) / "approved"
            approved.mkdir()
            outside = Path(temp) / "private.csv"
            outside.write_text(fixture("frames.csv"))
            link = approved / "escape.csv"
            try:
                link.symlink_to(outside)
            except OSError:
                self.skipTest("Creating test symlinks requires Windows developer mode or privilege")
            with self.assertRaises(EvidenceError):
                ApprovedPaths(ROOT, [approved]).resolve_import(str(link))

    @unittest.skipUnless(os.name == "nt", "Windows junction test")
    def test_junction_escape(self):
        with tempfile.TemporaryDirectory() as temp:
            approved = Path(temp) / "approved"
            approved.mkdir()
            outside = Path(temp) / "outside"
            outside.mkdir()
            (outside / "frames.csv").write_text(fixture("frames.csv"))
            link = approved / "escape"
            subprocess.run(["cmd.exe", "/c", "mklink", "/J", str(link), str(outside)],
                           check=True, capture_output=True, timeout=5)
            try:
                with self.assertRaises(EvidenceError):
                    ApprovedPaths(ROOT, [approved]).resolve_import(str(link / "frames.csv"))
            finally:
                os.rmdir(link)

    def test_document_aliases_and_unknown_alias(self):
        for alias, name in (("entry", "AGENTS.md"), ("architecture", "ARCHITECTURE.md"), ("performance", "PERFORMANCE.md")):
            doc = self.service.get_architecture(document=alias)["documents"][0]
            self.assertEqual(doc["path"], name)
            self.assertIn("text", doc)
        with self.assertRaises(EvidenceError):
            self.service.get_architecture(document="../private")

    def test_cache_ids_and_pagination(self):
        result = self.service.read_stats({"import_path": str(FIXTURES / "frames.csv")}, limit=2)
        identifier = result["artifact"]["artifact_id"]
        self.assertEqual(len(result["page"]["items"]), 2)
        self.assertEqual(result["page"]["next_cursor"], 2)
        again = self.service.read_stats({"artifact_id": identifier}, cursor=2, limit=2)
        self.assertEqual(again["artifact"], result["artifact"])
        self.assertEqual(result["validity"]["acceptance"], "not_checked")

    def test_benchmark_association_verified(self):
        result = self.service.read_benchmarks({"import_path": str(FIXTURES / "benchmark.log")})
        self.assertEqual(result["blocks"][0]["association_status"], "validated")

    def test_benchmark_reference_cannot_escape(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "benchmark.log"
            path.write_text(fixture("benchmark.log").replace("file=frames.csv", "file=../private.csv"))
            service = EvidenceService(ROOT, [temp])
            with self.assertRaises(EvidenceError):
                service.read_benchmarks({"import_path": str(path)})

    def test_imports_never_pass_comparison(self):
        source = {"import_path": str(FIXTURES / "frames.csv")}
        result = self.service.compare_runs(source, source)
        self.assertEqual(result["comparability"], "incomparable")
        self.assertEqual(result["performance"], "not_checked")
        self.assertTrue(result["deltas"])

    def test_missing_build_does_not_break_documents(self):
        with tempfile.TemporaryDirectory() as temp:
            service = EvidenceService(ROOT)
            service.paths.runtime = Path(temp) / "missing"
            self.assertTrue(service.get_architecture(document="entry")["documents"])
            status = service.server_status()
            self.assertFalse(status["runtime"]["exists"])
            self.assertFalse(status["capabilities"]["game_launch"])

    def test_cached_snapshot_does_not_change_with_original_file(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "frames.csv"
            path.write_text(fixture("frames.csv"))
            service = EvidenceService(ROOT, [temp])
            first = service.read_stats({"import_path": str(path)})
            path.write_text("changed")
            cached = service.read_stats({"artifact_id": first["artifact"]["artifact_id"]})
            self.assertEqual(first["artifact"], cached["artifact"])
            self.assertEqual(cached["summary"]["fps"], 50)

    def test_utf16_and_crlf_import(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "benchmark.log"
            raw = fixture("benchmark.log").replace("\n", "\r\n").encode("utf-16")
            path.write_bytes(raw)
            service = EvidenceService(ROOT, [temp])
            result = service.read_benchmarks({"import_path": str(path)})
            self.assertEqual(result["blocks"][0]["header"]["frames"], 5)
            self.assertEqual(result["artifact"]["byte_count"], len(raw))

    def test_scenarios_are_not_launch_capabilities(self):
        result = self.service.list_scenarios()
        self.assertEqual(len(result["scenarios"]), 6)
        self.assertTrue(all(not row["launch_supported"] for row in result["scenarios"]))


class ComparisonTests(unittest.TestCase):
    def record(self):
        return {"identity": {key: "matching" for key in IDENTITY_FIELDS}, "validity": "valid",
                "provenance_verified": True, "aggregation_kind": "per_frame", "map": "maps/start.bsp",
                "metrics": {"mean_ms": 20, "fps": 50, "calls_raster": 100, "cache_misses": 0}}

    def test_compatible_directions_zero_baseline(self):
        a = self.record()
        b = copy.deepcopy(a)
        b["metrics"].update(mean_ms=10, fps=100, calls_raster=80, cache_misses=1)
        result = compare_records(a, b)
        self.assertEqual(result["comparability"], "compatible")
        metrics = {item["name"]: item for item in result["deltas"]}
        self.assertEqual(metrics["mean_ms"]["delta_percent"], -50)
        self.assertEqual(metrics["calls_raster"]["direction"], "diagnostic")
        self.assertIsNone(metrics["cache_misses"]["delta_percent"])
        self.assertEqual(result["visual"], "not_checked")

    def test_elapsed_capture_time_is_not_cost_direction(self):
        record = self.record()
        record["metrics"]["time_ms"] = 100
        result = compare_records(record, record)
        value = next(item for item in result["deltas"] if item["name"] == "time_ms")
        self.assertEqual(value["direction"], "diagnostic")

    def test_missing_and_mismatched_identity(self):
        for field in IDENTITY_FIELDS:
            b = self.record()
            b["identity"][field] = None
            self.assertEqual(compare_records(self.record(), b)["comparability"], "incomparable")
            b["identity"][field] = "different"
            self.assertEqual(compare_records(self.record(), b)["comparability"], "incomparable")

    def test_missing_metric_is_not_pass(self):
        b = self.record()
        del b["metrics"]["fps"]
        result = compare_records(self.record(), b)
        self.assertEqual(result["comparability"], "incomparable")
        self.assertEqual(result["missing_metrics"], ["fps"])

    def test_matched_timing_policy_separate_from_visual_acceptance(self):
        a, b = self.record(), self.record()
        a['metrics']['p95_ms'] = 25
        b['metrics']['p95_ms'] = 25
        result = compare_records(a, b)
        self.assertEqual(result['performance'], 'within_5pct_tolerance')
        self.assertEqual(result['visual'], 'not_checked')
        b['metrics']['p95_ms'] = 30
        self.assertEqual(compare_records(a, b)['performance'], 'regression')

    def test_nonfinite_metrics_rejected(self):
        b = self.record()
        b["metrics"]["fps"] = float("nan")
        with self.assertRaises(EvidenceError):
            compare_records(self.record(), b)

    def test_comparison_overflow_rejected(self):
        a, b = self.record(), self.record()
        a["metrics"]["mean_ms"] = 1e-308
        b["metrics"]["mean_ms"] = 1e308
        with self.assertRaises(EvidenceError):
            compare_records(a, b)


if __name__ == "__main__":
    unittest.main()
