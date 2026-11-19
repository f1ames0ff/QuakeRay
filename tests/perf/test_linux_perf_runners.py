"""Unit tests for Linux QuakeRay perf runners, machine guard, and asset staging."""

from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

import machine_guard as mg
import run_map_suite
import run_menu
import run_place
from run_place import parse_place_blocks
import run_stress
from run_stress import validate_profiler_windows, validate_settings_line


XRANDR_SAMPLE = """\
Screen 0: minimum 320 x 200, current 3840 x 2160, maximum 16384 x 16384
DisplayPort-0 connected primary 3840x2160+0+0 (normal left inverted right x axis y axis) 697mm x 392mm
   3840x2160    165.06 +  60.00 + 144.05*   95.03
   2560x1440    143.97
DisplayPort-1 disconnected (normal left inverted right x axis y axis)
HDMI-A-0 connected 1920x1080+3840+0 (normal left inverted right x axis y axis) 527mm x 296mm
   1920x1080     60.00*+
"""

WMCTRL_SAMPLE = """\
0x03400004  0 2409   host App Title With Spaces
0x0bc00007  0 400417 host QuakeRay
"""


class LinuxPerfRunnerTests(unittest.TestCase):
    def test_xrandr_and_x11_parsers(self) -> None:
        displays = mg.parse_xrandr_output(XRANDR_SAMPLE)
        self.assertEqual(len(displays), 2)
        self.assertEqual(displays[0]["Name"], "DisplayPort-0")
        self.assertTrue(displays[0]["Primary"])
        self.assertEqual((displays[0]["Width"], displays[0]["Height"], displays[0]["RefreshHz"]), (3840, 2160, 144))
        self.assertEqual(displays[1]["Name"], "HDMI-A-0")
        self.assertFalse(displays[1]["Primary"])
        self.assertEqual((displays[1]["Width"], displays[1]["Height"], displays[1]["RefreshHz"]), (1920, 1080, 60))

        windows = mg.parse_wmctrl_windows(WMCTRL_SAMPLE)
        self.assertEqual(len(windows), 2)
        self.assertEqual(windows[0]["win_id"], "0x3400004")
        self.assertEqual(windows[0]["pid"], 2409)
        self.assertEqual(windows[0]["title"], "App Title With Spaces")
        self.assertEqual(windows[1]["win_id"], "0xbc00007")
        self.assertEqual(windows[1]["pid"], 400417)

        self.assertEqual(mg.parse_xprop_active_window("_NET_ACTIVE_WINDOW(WINDOW): window id # 0x0bc00007\n"), "0xbc00007")
        self.assertIsNone(mg.parse_xprop_active_window("_NET_ACTIVE_WINDOW(WINDOW): window id # 0x0\n"))
        self.assertEqual(mg.parse_xprop_pid("_NET_WM_PID(CARDINAL) = 400417\n"), 400417)
        self.assertIsNone(mg.parse_xprop_pid("_NET_WM_PID:  not found.\n"))

    def test_proc_stat_and_drm_fdinfo_gpu_busy(self) -> None:
        self.assertEqual(mg.parse_proc_stat_ppid_and_state("1234 (weird ) name) S 42 1234 0"), ("S", 42))

        fdinfo_text = (
            "pos:\t0\n"
            "drm-driver:\tamdgpu\n"
            "drm-client-id:\t23\n"
            "drm-engine-gfx:\t1000000000 ns\n"
            "drm-engine-compute:\t250000000 ns\n"
        )
        self.assertEqual(mg.parse_drm_fdinfo(fdinfo_text), ("23", 1250000000))

        snapshots = [
            {(1891, "23"): ("cinnamon", 1_000_000_000), (9999, "24"): ("game_bin", 2_000_000_000)},
            {(1891, "23"): ("cinnamon", 1_900_000_000), (9999, "24"): ("game_bin", 2_450_000_000)},
        ]
        clocks = iter([10.0, 11.0])
        busy = mg.get_gpu_busy_processes(
            threshold_pct=15.0,
            sample_ms=0,
            sampler=lambda: snapshots.pop(0),
            sleep_fn=lambda _: None,
            clock_fn=lambda: next(clocks),
        )
        self.assertEqual(busy, [{"pid": 9999, "comm": "game_bin", "gpu_pct": 45.0}])

    def test_machine_guard_exclusive_flock(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            lock_path = Path(tmp) / "guard.lock"
            with mg.MachineGuard(timeout_seconds=0.1, lock_path=lock_path, check_idle=False):
                with self.assertRaisesRegex(RuntimeError, "MACHINE_BUSY"):
                    with mg.MachineGuard(timeout_seconds=0.0, lock_path=lock_path, check_idle=False):
                        pass
            with mg.MachineGuard(timeout_seconds=0.1, lock_path=lock_path, check_idle=False):
                pass

    def test_budget_helpers(self) -> None:
        self.assertTrue(mg.test_stress_budget(None, 50.0, now=10.0))
        self.assertTrue(mg.test_stress_budget(30.0, 20.0, now=10.0))
        self.assertFalse(mg.test_stress_budget(29.9, 20.0, now=10.0))
        self.assertEqual(mg.get_stress_deadline(None, 25.0, now=10.0), 35.0)
        self.assertEqual(mg.get_stress_deadline(20.0, 25.0, now=10.0), 20.0)
        self.assertEqual(mg.get_stress_exit_timeout(20.0, now=10.0), 10000)
        self.assertEqual(mg.get_stress_exit_timeout(50.0, now=10.0), 15000)
        self.assertEqual(mg.get_stress_exit_timeout(5.0, now=10.0), 0)

    def test_stage_ad_assets_and_inventory(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ad_src = root / "ad_src"
            saves_src = root / "saves_src"
            mats_src = root / "mats_src"
            outside_file = root / "outside.pak"
            runtime = root / "runtime"
            ad_src.mkdir()
            saves_src.mkdir()
            mats_src.mkdir()
            (runtime / "id1").mkdir(parents=True)
            (runtime / "ad").mkdir(parents=True)

            (runtime / "id1/pak0.pak").write_bytes(b"ID1PAK")
            (runtime / "id1/config.cfg").write_bytes(b"// generated ignored\n")
            outside_file.write_bytes(b"EXT")
            (runtime / "ad/pak0.pak").symlink_to(outside_file)

            (ad_src / "pak0.pak").write_bytes(b"PACK0")
            (ad_src / "pak1.pak").write_bytes(b"PACK1")
            (ad_src / "pak2.pak").write_bytes(b"PACK2")
            (ad_src / "quake.rc").write_text("exec default.cfg\n", encoding="ascii")
            (saves_src / "qr_fuma_start.sav").write_bytes(b"SAV0")
            (mats_src / "qray.materials.yaml").write_text("materials: []\n", encoding="ascii")

            first = mg.stage_ad_assets(runtime, ad_dir=ad_src, saves_dir=saves_src, materials_dir=mats_src)
            self.assertEqual(first["pak0.pak"], "hardlinked")
            self.assertEqual(first["qr_fuma_start.sav"], "hardlinked")
            self.assertEqual(first["qray.materials.yaml"], "hardlinked")
            self.assertFalse((runtime / "ad/pak0.pak").is_symlink())
            self.assertEqual((runtime / "ad/pak0.pak").stat().st_ino, (ad_src / "pak0.pak").stat().st_ino)

            second = mg.stage_ad_assets(runtime, ad_dir=ad_src, saves_dir=saves_src, materials_dir=mats_src)
            self.assertEqual(second["pak0.pak"], "skipped")
            self.assertEqual(second["qr_fuma_start.sav"], "skipped")

            inventory = mg.build_asset_inventory(runtime)
            files = [entry["File"] for entry in inventory]
            self.assertIn("id1/pak0.pak", files)
            self.assertIn("ad/pak0.pak", files)
            self.assertIn("ad/qr_fuma_start.sav", files)
            self.assertNotIn("id1/config.cfg", files)

    def test_validate_settings_line_and_profiler_windows(self) -> None:
        base_settings = (
            "settings rt_upscale_fsr31=3 rt_sky_clouds_quality=1 vid=3840x2160@144 "
            "vsync=1 vid_maxframelatency=1 fov=90 rt_enable_pvs=0 sv_novis=1 rt_restir=0 "
            "rt_nee_samples=2 rt_restir_candidates=8 rt_reflrefr_depth=8 rt_dtal_groups=0 "
            "rt_dtal_spacing=128 r_fteparticles=0 rt_gi_level=1 rt_sky_godrays=1 "
            "rt_sky_godrays_quality=1 rt_sky_clouds=1 rt_physical_sky=1 rt_physical_sun=1 "
            "rt_sky_nee=1 rt_denoiser=1 r_particles=1 r_smoke=1 rt_renderscale=0 "
            "rt_upscale_dlss=0 rt_dtal_lod=0 rt_q2_lightstats=1"
        )
        validate_settings_line(base_settings, 3, "medium", 1, 1, 0, False)
        with self.assertRaises(ValueError):
            validate_settings_line(base_settings.replace("vid=3840x2160@144", "vid=1920x1080@144"), 3, "medium", 1, 1, 0, False)
        with self.assertRaises(ValueError):
            validate_settings_line(base_settings.replace("rt_gi_level=1", "rt_gi_level=0"), 3, "medium", 1, 1, 0, False)
        validate_settings_line(base_settings.replace("rt_gi_level=1", "rt_gi_level=0"), 3, "medium", 1, 1, 0, True)

        with tempfile.TemporaryDirectory() as tmp:
            dump = Path(tmp) / "stats-1.dump"
            dump.write_text(
                "# header\n"
                "cpu.window_id,cpu.window_frames,cpu.renderer_samples,cpu.qrDrawFrame_avg_ms\n"
                "1,30,30,4.5\n"
                "2,30,30,4.6\n",
                encoding="utf-8",
            )
            win = validate_profiler_windows(dump)
            self.assertEqual(win["PublishedWindows"], 2)
            self.assertTrue(win["Complete"])

            bad_dump = Path(tmp) / "stats-bad.dump"
            bad_dump.write_text(
                "cpu.window_id,cpu.window_frames,cpu.renderer_samples,cpu.qrDrawFrame_avg_ms\n"
                "1,30,29,4.5\n"
                "2,30,30,4.6\n",
                encoding="utf-8",
            )
            with self.assertRaises(ValueError):
                validate_profiler_windows(bad_dump)

    def test_check_ui_capture(self) -> None:
        good_rows = [
            {
                "gpu.frame_ms": "0.30",
                "gpu.clouds_ms": "0.00",
                "gpu.sky_ms": "0.00",
                "gpu.primary_ms": "0.00",
                "gpu.decals_ms": "0.00",
                "gpu.godrays_ms": "0.00",
                "gpu.reflrefr_ms": "0.00",
                "gpu.reflgodr_ms": "0.00",
                "gpu.gradient_ms": "0.00",
                "gpu.direct_ms": "0.00",
                "gpu.indirect_ms": "0.00",
                "gpu.compose_ms": "0.00",
                "gpu.upscale_ms": "0.00",
                "gpu.post_ms": "0.00",
                "gpu.ui_ms": "0.25",
                "cpu.draw.legacy_AS_max_ms": "0.00",
                "cpu.draw.staging_max_ms": "0.00",
                "cpu.draw.scene_record_max_ms": "0.00",
                "cpu.draw.compose_max_ms": "0.00",
                "cpu.draw.upscale_max_ms": "0.00",
                "cpu.draw.post_max_ms": "0.00",
            }
        ]
        run_menu.check_ui_capture(good_rows, "good.dump")

        bad_gpu = [dict(good_rows[0], **{"gpu.primary_ms": "0.10"})]
        with self.assertRaisesRegex(ValueError, "primary pass"):
            run_menu.check_ui_capture(bad_gpu, "bad.dump")

        bad_overflow = [dict(good_rows[0], **{"gpu.ui_ms": "0.50"})]
        with self.assertRaisesRegex(ValueError, "exceeds its frame total"):
            run_menu.check_ui_capture(bad_overflow, "bad2.dump")

        bad_cpu = [dict(good_rows[0], **{"cpu.draw.scene_record_max_ms": "0.20"})]
        with self.assertRaisesRegex(ValueError, "scene_record CPU work"):
            run_menu.check_ui_capture(bad_cpu, "bad3.dump")

    def test_runner_defaults_and_menu_dump_parser(self) -> None:
        self.assertEqual(run_stress.parse_args([]).fullscreen, 2)
        self.assertEqual(run_map_suite.parse_args([]).fullscreen, 2)
        menu_args = run_menu.parse_args(["--smoke"])
        self.assertFalse(menu_args.interactive)
        self.assertTrue(run_menu.parse_args(["--interactive"]).interactive)

        menu_cfg = run_menu._build_scripted_cfg("t", 4, True)
        self.assertIn("cmd pext", menu_cfg)
        self.assertLess(menu_cfg.index("map start"), menu_cfg.index("cmd pext"))
        self.assertLess(menu_cfg.index("cmd pext"), menu_cfg.index("menu_main", menu_cfg.index("map start")))
        self.assertIn("menu_main; toggleconsole; quit", menu_cfg)

        stress_cfg = run_stress._build_fixture_text(
            rt_profile="medium",
            fsr=3,
            overrides=[],
            fullscreen=2,
            initial_stats_level=3,
            stats_level=3,
            enable_stats_after_warmup=False,
            scheduled=True,
            scheduled_warmup_frames=120,
            scheduled_capture_frames=180,
            map_command="load qr_fuma_start",
            save="qr_fuma_start",
            run_id="abcd1234",
        )
        self.assertIn("cmd pext", stress_cfg)
        self.assertLess(stress_cfg.index("load qr_fuma_start"), stress_cfg.index("cmd pext"))
        self.assertLess(stress_cfg.index("cmd pext"), stress_cfg.index("mapname"))
        self.assertIn("menu_main; toggleconsole; quit", stress_cfg)

        map_cfg = run_stress._build_fixture_text(
            rt_profile="medium",
            fsr=3,
            overrides=[],
            fullscreen=2,
            initial_stats_level=3,
            stats_level=3,
            enable_stats_after_warmup=False,
            scheduled=False,
            scheduled_warmup_frames=0,
            scheduled_capture_frames=0,
            map_command="map ad_tfuma",
            save="ad_tfuma",
            run_id="abcd1234",
        )
        self.assertIn('bind F7 "menu_close;', map_cfg)
        self.assertIn('bind F9 "menu_close; echo QR_RELOAD_abcd1234"', map_cfg)

        console_sample = (
            "rt_stats_dump_end: 9 samples over 4.3 s -> /tmp/rt/id1/stats-20261119-163011.dump\n"
            "rt_stats_dump_end (the 30 s are up): 20 samples over 30.0 s -> id1/stats-20261119-163045.dump\n"
        )
        parsed = run_menu.parse_stats_dump_paths(console_sample, base_dir=Path("/tmp/rt"))
        self.assertEqual(
            parsed,
            [
                Path("/tmp/rt/id1/stats-20261119-163011.dump"),
                Path("/tmp/rt/id1/stats-20261119-163045.dump"),
            ],
        )

    def test_parse_place_blocks(self) -> None:
        lines = [
            "# rt_bench test demo=maps/ad_swampy.bsp frames=120 seconds=4.00 fps=30.0 interrupted=0",
            "settings rt_cluster_sampling=1 vid=1280x720@144",
            "cpu.slot frame avg_ms=33.100 max_ms=40.200",
            "cpu.slot clust lists avg_ms=1.200 max_ms=2.300",
            "cpu.cluster lists hits=100 misses=5 set=2 move=3 other=0",
        ]
        blocks = parse_place_blocks(lines)
        self.assertEqual(len(blocks), 1)
        self.assertEqual(blocks[0]["demo"], "maps/ad_swampy.bsp")
        self.assertEqual(blocks[0]["sampling"], 1)
        self.assertEqual(blocks[0]["slots"]["frame"], ("33.100", "40.200"))
        self.assertEqual(blocks[0]["cluster_lists"]["hits"], 100)

    def test_send_key_and_wait_marker_retry_and_ack(self) -> None:
        class _FakeProc:
            pid = 1234

            def poll(self) -> None:
                return None

        class _FakeX11:
            def __init__(self, on_send: object) -> None:
                self.sent: list[str] = []
                self._on_send = on_send

            def find_window_for_pid(self, pid: int) -> str:
                return "0x123"

            def force_focus(self, win_id: str, pid: int, retries: int = 5, sleep_s: float = 0.2) -> bool:
                return True

            def send_key(self, win_id: str, pid: int, key_name: str) -> None:
                self.sent.append(key_name)
                self._on_send(len(self.sent))

        class _FakeWaiter:
            def __init__(self) -> None:
                self.counts: dict[str, int] = {}
                self.wait_calls: list[tuple[str, int]] = []

            def count_marker(self, marker: str) -> int:
                return self.counts.get(marker, 0)

            def wait_for_marker(
                self, proc: object, marker: str, deadline: float, stderr_path: Path, min_count: int = 1
            ) -> None:
                self.wait_calls.append((marker, min_count))
                self.counts[marker] = min_count

        with tempfile.TemporaryDirectory() as tmp:
            stderr_path = Path(tmp) / "stderr.log"

            waiter1 = _FakeWaiter()
            x11_1 = _FakeX11(
                lambda n: waiter1.counts.__setitem__("==== RELOAD_S0", 1) if n == 2 else None
            )
            win = run_place._send_key_and_wait_marker(
                x11_1,  # type: ignore[arg-type]
                waiter1,  # type: ignore[arg-type]
                _FakeProc(),  # type: ignore[arg-type]
                "0x1",
                "F5",
                "==== RELOAD_S0",
                5.0,
                stderr_path,
                ack_marker="Loading game from",
                retry_after_s=0.15,
                max_attempts=3,
            )
            self.assertEqual(win, "0x123")
            self.assertEqual(x11_1.sent, ["F5", "F5"])

            waiter2 = _FakeWaiter()
            waiter2.counts["Loading game from"] = 2
            x11_2 = _FakeX11(
                lambda n: waiter2.counts.__setitem__("Loading game from", 3) if n == 1 else None
            )
            run_place._send_key_and_wait_marker(
                x11_2,  # type: ignore[arg-type]
                waiter2,  # type: ignore[arg-type]
                _FakeProc(),  # type: ignore[arg-type]
                "0x1",
                "F5",
                "==== RELOAD_S0",
                5.0,
                stderr_path,
                ack_marker="Loading game from",
                retry_after_s=0.15,
                max_attempts=3,
            )
            self.assertEqual(x11_2.sent, ["F5"])
            self.assertEqual(waiter2.wait_calls, [("==== RELOAD_S0", 1)])


if __name__ == "__main__":
    unittest.main()
