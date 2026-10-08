import csv
import io
import unittest

from analyze_stress import parse_capture, percentile, summarize


def fixture(paused=0, key_game=1, dropped=0):
    stream = io.StringIO()
    stream.write(f"# rt_bench_frames demo=maps/start.bsp frames=5 samples=5 dropped={dropped}\n")
    columns = ["frame", "time_ms", "interval_ms", "client_time", "key_game", "paused", "signon",
               "ui_only", "gpu_valid", "gpu.frame_ms", "cpu.ents_ms"]
    writer = csv.DictWriter(stream, fieldnames=columns)
    writer.writeheader()
    for index in range(5):
        writer.writerow({"frame": index, "time_ms": 10 + index * 20, "interval_ms": 20 if index else 0,
                         "client_time": 1 + index * 0.02, "key_game": key_game, "paused": paused,
                         "signon": 4, "ui_only": 0, "gpu_valid": 1, "gpu.frame_ms": 12, "cpu.ents_ms": 7})
    return stream.getvalue()


class StressCaptureTests(unittest.TestCase):
    def test_percentiles(self):
        self.assertEqual(percentile([1, 3, 2, 4], 0.5), 2.5)
        self.assertEqual(percentile([7], 0.99), 7)
        self.assertAlmostEqual(percentile([0, 100], 0.95), 95)
        with self.assertRaises(ValueError):
            percentile([], 0.5)

    def test_full_frame_intervals(self):
        result = summarize(fixture(), 1000 / 60)
        self.assertEqual(result["intervals"], 4)
        self.assertEqual(result["fps"], 50)
        self.assertEqual(result["p99_ms"], 20)
        self.assertEqual(result["over_budget_percent"], 100)
        self.assertEqual(result["cpu_mean_ms"]["cpu.ents_ms"], 7)
        self.assertEqual(result["gpu_snapshot_mean_ms"]["gpu.frame_ms"], 12)

    def test_target_quality_budget(self):
        self.assertEqual(summarize(fixture(), 1000 / 45)["over_budget_percent"], 0)

    def test_reject_paused_or_menu_capture(self):
        for text in (fixture(paused=1), fixture(key_game=0)):
            with self.assertRaises(ValueError):
                parse_capture(text)
            self.assertEqual(parse_capture(text, allow_paused=True)[0], "maps/start.bsp")

    def test_reject_incomplete_capture(self):
        with self.assertRaises(ValueError):
            parse_capture(fixture(dropped=1))
        with self.assertRaises(ValueError):
            parse_capture(fixture().replace("frames=5", "frames=6"))

    def test_reject_bad_clock_and_nonfinite(self):
        for text in (fixture().replace("20,1.02", "19,1.02"), fixture().replace("12,7", "nan,7")):
            with self.assertRaises(ValueError):
                parse_capture(text)


if __name__ == "__main__":
    unittest.main()
