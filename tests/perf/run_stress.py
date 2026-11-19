#!/usr/bin/env python3
"""Linux stress and audit capture runner for QuakeRay (ports tests/perf/run_stress.ps1)."""

from __future__ import annotations

import argparse
import csv
from datetime import datetime
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
from typing import Any
import uuid

PERF_DIR = Path(__file__).resolve().parent
if str(PERF_DIR) not in sys.path:
    sys.path.insert(0, str(PERF_DIR))

from machine_guard import (  # noqa: E402
    ConsoleWaiter,
    MachineGuard,
    REPO_ROOT,
    X11Controller,
    build_asset_inventory,
    default_basedir,
    display_env,
    get_active_displays,
    get_gpu_busy_processes,
    get_heavy_processes,
    get_process_ancestor_ids,
    get_stress_deadline,
    get_stress_exit_timeout,
    kill_process_gracefully,
    sha256_file,
    stage_ad_assets,
    test_stop_requested,
    test_stress_budget,
    wait_for_idle,
)

KNOWN_SAVES = ("qr_fuma_start", "qr_ad_start", "qr_gpu_heavy", "qr_swampy_start", "qr_ad_swamp")
EXPECTED_MAPS = {
    "qr_fuma_start": "ad_tfuma",
    "qr_ad_start": "start",
    "qr_gpu_heavy": "ad_swampy",
    "qr_swampy_start": "ad_swampy",
    "qr_ad_swamp": "ad_swampy",
}
PRESET_FSR = {"quality": 2, "balanced": 3, "performance": 4, "ultra": 5}
MEDIUM_PROFILE_SETTINGS = (
    "rt_gi_level=1",
    "rt_sky_godrays=1",
    "rt_sky_godrays_quality=1",
    "rt_sky_clouds=1",
    "rt_physical_sky=1",
    "rt_physical_sun=1",
    "rt_sky_nee=1",
    "rt_denoiser=1",
    "r_particles=1",
    "r_smoke=1",
    "rt_renderscale=0",
    "rt_upscale_dlss=0",
    "rt_dtal_lod=0",
    "rt_q2_lightstats=1",
)


def validate_settings_line(
    settings_line: str,
    fsr: int,
    rt_profile: str,
    expected_vsync: int,
    expected_max_frame_latency: int,
    expected_restir: int,
    allow_profile_overrides: bool = False,
) -> None:
    if not settings_line:
        raise ValueError("Missing settings line in benchmark capture.")
    expected_clouds = 1 if rt_profile == "medium" else 0
    if (
        "vid=3840x2160@" not in settings_line
        or not re.search(rf"(?:^| )rt_upscale_fsr31={fsr}(?: |$)", settings_line)
        or not re.search(rf"(?:^| )rt_sky_clouds_quality={expected_clouds}(?: |$)", settings_line)
    ):
        raise ValueError(
            "Effective resolution/FSR/cloud quality does not match the requested target: "
            f"{settings_line}"
        )

    expected_tokens = [
        f"vsync={expected_vsync}",
        f"vid_maxframelatency={expected_max_frame_latency}",
        "fov=90",
        "rt_enable_pvs=0",
        "sv_novis=1",
        f"rt_restir={expected_restir}",
        "rt_nee_samples=2",
        "rt_restir_candidates=8",
        "rt_reflrefr_depth=8",
        "rt_dtal_groups=0",
        "rt_dtal_spacing=128",
        "r_fteparticles=0",
    ]
    if rt_profile == "medium" and not allow_profile_overrides:
        expected_tokens.extend(MEDIUM_PROFILE_SETTINGS)

    for token in expected_tokens:
        if not re.search(rf"(?:^| ){re.escape(token)}(?: |$)", settings_line):
            raise ValueError(f"Effective settings do not contain the standard '{token}'.")


def validate_profiler_windows(dump_path: Path) -> dict[str, Any]:
    lines = [line for line in dump_path.read_text(encoding="utf-8", errors="replace").splitlines() if line and not line.startswith("#")]
    rows = list(csv.DictReader(lines))
    published = [row for row in rows if re.fullmatch(r"[1-9][0-9]*", (row.get("cpu.window_id") or "").strip())]
    ids = sorted({int(row["cpu.window_id"]) for row in published})
    if len(ids) < 2:
        raise ValueError("Profiler windows did not advance during capture; rebuild or investigate publication.")
    for row in published:
        window_frames = int(row["cpu.window_frames"])
        renderer_samples = int(row["cpu.renderer_samples"])
        avg_ms = float(row["cpu.qrDrawFrame_avg_ms"])
        if window_frames <= 0 or window_frames != renderer_samples or avg_ms <= 0.0:
            raise ValueError("Profiler window has an incomplete result set or an invalid denominator.")
    return {
        "PublishedWindows": len(ids),
        "FirstWindowFrames": int(published[0]["cpu.window_frames"]),
        "FirstRendererSamples": int(published[0]["cpu.renderer_samples"]),
        "Complete": True,
    }


def _git_output(args: list[str], default: str = "") -> str:
    try:
        proc = subprocess.run(["git", "-C", str(REPO_ROOT), *args], capture_output=True, text=True, check=False)
        if proc.returncode == 0:
            return proc.stdout.strip()
    except OSError:
        pass
    return default


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run QuakeRay Linux stress/audit capture.")
    parser.add_argument("--save", default="qr_fuma_start", help="Save scenario name (default: qr_fuma_start)")
    parser.add_argument("--game", choices=("id1", "mg1", "mg3", "ad"), default="ad")
    parser.add_argument("--map", dest="map_name", default="", help="Run a single map instead of a save scenario")
    parser.add_argument("--preset", choices=("balanced", "quality", "performance", "ultra"), default="balanced")
    parser.add_argument("--rt-profile", choices=("audit", "medium"), default="audit")
    parser.add_argument("--seconds", type=int, default=10)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--repeats", type=int, default=1)
    parser.add_argument("--max-run-seconds", type=int, default=300)
    parser.add_argument("--basedir", default="", help="Runtime build directory containing quakeray")
    parser.add_argument("--ad-dir", default="", help="Source Arcane Dimensions directory containing pak0..2.pak")
    parser.add_argument("--ad-saves-dir", default="", help="Source directory containing canonical AD .sav files")
    parser.add_argument("--fullscreen", type=int, choices=(0, 1, 2), default=2)
    parser.add_argument("--tag", default="baseline")
    parser.add_argument("--override", action="append", default=[], help="Extra console override line (repeatable)")
    parser.add_argument("--expected-vsync", type=int, choices=(1, 2, 3), default=1)
    parser.add_argument("--expected-max-frame-latency", type=int, choices=(1,), default=1)
    parser.add_argument("--expected-restir", type=int, choices=(0, 1), default=0)
    parser.add_argument("--stats-level", type=int, choices=(0, 1, 2, 3), default=0)
    parser.add_argument("--no-sound", action="store_true")
    parser.add_argument("--enable-stats-after-warmup", action="store_true")
    parser.add_argument("--scheduled", action="store_true")
    parser.add_argument("--move-during-capture", action="store_true")
    parser.add_argument("--allow-profile-overrides", action="store_true")
    parser.add_argument("--screenshot", action="store_true")
    args = parser.parse_args(argv)

    if not (2 <= args.seconds <= 25):
        parser.error("--seconds must be in 2..25")
    if not (2 <= args.warmup <= 30):
        parser.error("--warmup must be in 2..30")
    if not (1 <= args.repeats <= 2):
        parser.error("--repeats must be in 1..2")
    if not (30 <= args.max_run_seconds <= 300):
        parser.error("--max-run-seconds must be in 30..300")
    if args.enable_stats_after_warmup and args.stats_level != 3:
        parser.error("--enable-stats-after-warmup requires --stats-level 3")
    if args.scheduled and args.move_during_capture:
        parser.error("--scheduled captures cannot inject movement input")
    if args.scheduled and args.screenshot:
        parser.error("--scheduled captures do not take interactive screenshots")
    if args.map_name:
        if not re.fullmatch(r"^[A-Za-z0-9_./-]+$", args.map_name):
            parser.error(f"Invalid --map name: {args.map_name!r}")
    elif args.save not in KNOWN_SAVES:
        parser.error(f"Unknown save scenario '{args.save}'; pass --map instead.")
    return args


def _build_fixture_text(
    *,
    rt_profile: str,
    fsr: int,
    overrides: list[str],
    fullscreen: int,
    initial_stats_level: int,
    stats_level: int,
    enable_stats_after_warmup: bool,
    scheduled: bool,
    scheduled_warmup_frames: int,
    scheduled_capture_frames: int,
    map_command: str,
    save: str,
    run_id: str,
) -> str:
    profile_cfg = PERF_DIR / ("qr_medium.cfg" if rt_profile == "medium" else "qr_audit_max.cfg")
    profile_text = profile_cfg.read_text(encoding="utf-8", errors="replace").rstrip("\r\n")
    start_dump = "rt_stats_dump_start;" if stats_level > 0 else ""
    end_dump = "rt_stats_dump_end;" if stats_level > 0 else ""
    start_stats = f"rt_stats {stats_level};" if enable_stats_after_warmup else ""

    if scheduled:
        startup_waits = "wait;" * 30
        warmup_waits = "wait;" * scheduled_warmup_frames
        capture_waits = "wait;" * scheduled_capture_frames
        flush_waits = "wait;" * 3
        tail = "\n".join(
            [
                startup_waits,
                map_command,
                "wait;wait;",
                "cmd pext",
                "wait;" * 30,
                f"echo QR_LOADED0_{run_id}",
                "mapname",
                "menu_close",
                f"echo QR_MAP_{run_id}",
                warmup_waits,
                f"{start_stats} rt_bench start; {start_dump} echo QR_START_{run_id}".strip(),
                capture_waits,
                f"rt_bench stop; {end_dump} echo QR_STOP_{run_id}".strip(),
                flush_waits,
                f"echo QR_QUIT_{run_id}",
                "menu_main; toggleconsole; quit",
            ]
        )
    else:
        f9_command = (
            f"menu_close; echo QR_RELOAD_{run_id}"
            if map_command.startswith("map ")
            else f"load {save}; wait; menu_close; echo QR_RELOAD_{run_id}"
        )
        tail = "\n".join(
            [
                f'bind F4 "{start_stats} rt_bench start; {start_dump} echo QR_START_{run_id}"',
                f'bind F5 "rt_bench stop; {end_dump} echo QR_STOP_{run_id}"',
                'bind F6 "screenshot"',
                f'bind F7 "menu_close; mapname; vid_describecurrentmode; echo QR_EFFECTIVE_{run_id}"',
                f'bind F8 "restart; echo QR_RESTART_{run_id}"',
                f'bind F9 "{f9_command}"',
                'bind F11 "menu_main; toggleconsole; quit"',
                map_command,
                f"echo QR_LOADED0_{run_id}",
                "mapname",
                f"echo QR_MAP_{run_id}",
            ]
        )

    parts = [
        profile_text,
        f"rt_upscale_fsr31 {fsr}",
        *overrides,
        "vid_unlock",
        f"vid_fullscreen {fullscreen}",
        "vid_width 3840",
        "vid_height 2160",
        "vid_restart",
        f"rt_stats {initial_stats_level}",
        tail,
    ]
    return "\n".join(line for line in parts if line != "") + "\n"


def _write_manifest(path: Path, manifest: dict[str, Any]) -> None:
    path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def _check_foreign_activity(own_pid: int, ancestors: set[int], check_gpu: bool = True) -> None:
    excluded = ancestors | {own_pid}
    foreign = get_heavy_processes(excluded_pids=excluded)
    if foreign:
        owners = ", ".join(f"{p['pid']}:{p['comm']}" for p in foreign)
        raise RuntimeError(f"MACHINE_BUSY: foreign heavy activity contaminated this capture: {owners}")
    if check_gpu:
        gpu_busy = get_gpu_busy_processes(excluded_pids=excluded, sample_ms=250)
        if gpu_busy:
            owners = ", ".join(f"{g['pid']}:{g['comm']}={g['gpu_pct']}%" for g in gpu_busy)
            raise RuntimeError(f"MACHINE_BUSY: foreign GPU activity contaminated this capture: {owners}")


def run_stress(args: argparse.Namespace) -> list[dict[str, str]]:
    shutdown_reserve = 15
    minimum_capture_seconds = args.warmup + args.seconds + shutdown_reserve
    if minimum_capture_seconds > args.max_run_seconds:
        raise ValueError("Warmup, capture and shutdown reserve cannot fit within --max-run-seconds.")

    basedir = Path(args.basedir).resolve() if args.basedir else default_basedir().resolve()
    exe = basedir / "quakeray"
    if not exe.is_file():
        raise FileNotFoundError(f"Missing Linux runtime binary: {exe}")

    if args.game == "ad" or args.ad_dir or args.ad_saves_dir:
        stage_ad_assets(
            basedir,
            ad_dir=args.ad_dir or None,
            saves_dir=args.ad_saves_dir or None,
        )

    game_dir = basedir / args.game
    game_dir.mkdir(parents=True, exist_ok=True)
    console_path = basedir / "qconsole.log"
    config_path = game_dir / "config.cfg"
    engine_pkz = basedir / "id1/qray.pkz"
    if not engine_pkz.is_file():
        raise FileNotFoundError(f"Missing engine asset archive: {engine_pkz}")

    saves = [args.map_name] if args.map_name else [args.save]
    save = saves[0]
    map_mode = bool(args.map_name) or save == "qr_swampy_start"
    expected_map = Path(args.map_name).name.removesuffix(".bsp") if args.map_name else EXPECTED_MAPS[save]

    if not map_mode and not (game_dir / f"{save}.sav").is_file():
        raise FileNotFoundError(f"Missing save: {game_dir / f'{save}.sav'}")

    scheduled_warmup_frames = int(math.ceil(args.warmup * 60.0)) if args.scheduled else 0
    scheduled_capture_frames = int(math.ceil(args.seconds * 60.0)) if args.scheduled else 0

    safe_tag = re.sub(r"[^a-zA-Z0-9_-]", "_", args.tag)
    run_tag = f"{safe_tag}-{datetime.now().strftime('%Y%m%d-%H%M%S')}-{uuid.uuid4().hex[:6]}"
    output = basedir / f"audit-{run_tag}"
    output.mkdir(parents=True, exist_ok=True)
    manifest_path = output / "manifest.json"

    results: list[dict[str, str]] = []
    run_deadline: float | None = None

    with MachineGuard():
        ancestors = get_process_ancestor_ids()
        active_displays = get_active_displays()
        working_changes = [line for line in _git_output(["status", "--short"]).splitlines() if line.strip()]
        manifest: dict[str, Any] = {
            "Timestamp": datetime.now().astimezone().isoformat(),
            "Revision": _git_output(["rev-parse", "HEAD"], default="unknown"),
            "WorkingChanges": working_changes,
            "Executable": str(exe),
            "ExecutableSha256": sha256_file(exe),
            "EngineAssetsSha256": sha256_file(engine_pkz),
            "Game": args.game,
            "Maps": [args.map_name] if args.map_name else [],
            "Saves": saves,
            "Presets": [args.preset],
            "RtProfile": args.rt_profile,
            "Seconds": args.seconds,
            "Warmup": args.warmup,
            "Fullscreen": args.fullscreen,
            "Repeats": args.repeats,
            "MaxRunSeconds": args.max_run_seconds,
            "StatsLevel": args.stats_level,
            "NoSound": bool(args.no_sound),
            "EnableStatsAfterWarmup": bool(args.enable_stats_after_warmup),
            "Scheduled": bool(args.scheduled),
            "ScheduledWarmupFrames": scheduled_warmup_frames,
            "ScheduledCaptureFrames": scheduled_capture_frames,
            "MoveDuringCapture": bool(args.move_during_capture),
            "AllowProfileOverrides": bool(args.allow_profile_overrides),
            "ExpectedVsync": args.expected_vsync,
            "ExpectedMaxFrameLatency": args.expected_max_frame_latency,
            "ExpectedRestir": args.expected_restir,
            "Overrides": list(args.override),
            "LoaderLayers": os.environ.get("VK_INSTANCE_LAYERS"),
            "LoaderLayersDisabled": os.environ.get("VK_LOADER_LAYERS_DISABLE"),
            "ActiveDisplays": active_displays,
            "Assets": [],
        }

        asset_names = ["pak0.pak", "pak1.pak", "pak2.pak", "qray.materials.yaml", "qray.lights.yaml"]
        if not map_mode:
            asset_names.append(f"{save}.sav")
        for asset in asset_names:
            asset_path = game_dir / asset
            if asset_path.is_file():
                manifest["Assets"].append({"File": asset, "Sha256": sha256_file(asset_path)})
        _write_manifest(manifest_path, manifest)

        inventory_entries = build_asset_inventory(basedir)
        inventory_path = output / "assets.json"
        inventory_path.write_text(json.dumps(inventory_entries), encoding="utf-8")
        manifest["AssetInventorySha256"] = sha256_file(inventory_path)
        manifest["AssetInventoryPolicy"] = 1
        _write_manifest(manifest_path, manifest)

        with X11Controller() as x11:
            for repeat in range(1, args.repeats + 1):
                if not test_stress_budget(run_deadline, minimum_capture_seconds):
                    print("Runtime budget reached; remaining repeats are deferred.")
                    break
                wait_for_idle(deadline=run_deadline, excluded_pids=ancestors)
                if not test_stress_budget(run_deadline, minimum_capture_seconds):
                    print("Runtime budget reached; remaining repeats are deferred.")
                    break

                run_id = uuid.uuid4().hex[:8]
                fixture = game_dir / f"qs_{run_id}.cfg"
                raw_label = f"{args.game}-{args.map_name}-{args.preset}-{repeat}" if args.map_name else f"{save}-{args.preset}-{repeat}"
                label = re.sub(r"[^A-Za-z0-9_.-]", "_", raw_label)
                saved_config = config_path.read_bytes() if config_path.is_file() else None
                old_dumps = set(game_dir.glob("stats-*.dump"))
                old_screenshots = set(game_dir.glob("screenshot*.png"))
                bench_path = game_dir / "benchmark.log"
                old_bench_lines = len(bench_path.read_text(encoding="utf-8", errors="replace").splitlines()) if bench_path.is_file() else 0

                fsr = PRESET_FSR[args.preset]
                initial_stats_level = 0 if args.enable_stats_after_warmup else args.stats_level
                map_command = f"map {expected_map}" if map_mode else f"load {save}"
                fixture_text = _build_fixture_text(
                    rt_profile=args.rt_profile,
                    fsr=fsr,
                    overrides=list(args.override),
                    fullscreen=args.fullscreen,
                    initial_stats_level=initial_stats_level,
                    stats_level=args.stats_level,
                    enable_stats_after_warmup=args.enable_stats_after_warmup,
                    scheduled=args.scheduled,
                    scheduled_warmup_frames=scheduled_warmup_frames,
                    scheduled_capture_frames=scheduled_capture_frames,
                    map_command=map_command,
                    save=save,
                    run_id=run_id,
                )
                fixture.write_text(fixture_text, encoding="ascii")
                shutil.copy2(fixture, output / f"{label}.cfg")

                if console_path.is_file():
                    console_path.unlink()

                stdout_path = output / f"{label}.stdout.log"
                stderr_path = output / f"{label}.stderr.log"
                proc: subprocess.Popen[bytes] | None = None
                move_key_held = False
                win_id: str | None = None

                try:
                    if not test_stress_budget(run_deadline, minimum_capture_seconds):
                        print("Preparation consumed the capture budget; remaining repeats are deferred.")
                        break
                    if run_deadline is None:
                        run_deadline = time.monotonic() + args.max_run_seconds

                    cmd = [
                        str(exe),
                        "-basedir",
                        ".",
                        "-game",
                        args.game,
                        "-window",
                        "-width",
                        "1280",
                        "-height",
                        "720",
                        "-condebug",
                        "+exec",
                        f"qs_{run_id}.cfg",
                    ]
                    if args.no_sound:
                        cmd.append("-nosound")

                    waiter = ConsoleWaiter(console_path)
                    with stdout_path.open("wb") as out_f, stderr_path.open("wb") as err_f:
                        proc = subprocess.Popen(
                            cmd,
                            cwd=str(basedir),
                            env=display_env(),
                            stdout=out_f,
                            stderr=err_f,
                        )

                    waiter.wait_for_marker(proc, f"QR_LOADED0_{run_id}", run_deadline, stderr_path)
                    waiter.wait_for_marker(proc, f"QR_MAP_{run_id}", run_deadline, stderr_path)

                    focus_losses = 0
                    if args.scheduled:
                        win_id = x11.find_window_for_pid(proc.pid)
                        if win_id:
                            x11.force_focus(win_id, proc.pid, retries=5, sleep_s=0.2)
                        waiter.wait_for_marker(proc, f"QR_START_{run_id}", run_deadline, stderr_path)
                        _check_foreign_activity(proc.pid, ancestors, check_gpu=True)

                        cap_deadline = get_stress_deadline(run_deadline - shutdown_reserve, args.max_run_seconds)
                        next_owner_check = time.monotonic()
                        while True:
                            if test_stop_requested():
                                raise RuntimeError("CANCELLED: owned runtime stopped; capture remains unaccepted.")
                            if waiter.count_marker(f"QR_STOP_{run_id}") >= 1:
                                break
                            if proc.poll() is not None:
                                raise RuntimeError(f"Runtime exited with code {proc.returncode} before QR_STOP_{run_id}")
                            if time.monotonic() >= cap_deadline:
                                raise RuntimeError(f"Timed out waiting for QR_STOP_{run_id}")
                            if time.monotonic() >= next_owner_check:
                                _check_foreign_activity(proc.pid, ancestors, check_gpu=True)
                                next_owner_check = time.monotonic() + 1.0
                            time.sleep(0.1)

                        _check_foreign_activity(proc.pid, ancestors, check_gpu=True)
                        exit_timeout_ms = get_stress_exit_timeout(run_deadline)
                        proc.wait(timeout=max(1.0, exit_timeout_ms / 1000.0))
                        if proc.returncode != 0:
                            raise RuntimeError(f"Runtime exit code {proc.returncode}")
                    else:
                        win_id = x11.wait_for_window(proc, timeout_s=max(5.0, min(30.0, run_deadline - time.monotonic())))
                        if not test_stress_budget(run_deadline, minimum_capture_seconds):
                            print("Loading consumed the capture budget; this attempt is deferred.")
                            if x11.force_focus(win_id, proc.pid, retries=5, sleep_s=0.2):
                                x11.send_key(win_id, proc.pid, "F11")
                            proc.wait(timeout=max(1.0, get_stress_exit_timeout(run_deadline) / 1000.0))
                            break

                        x11.force_focus(win_id, proc.pid, retries=8, sleep_s=0.25)
                        waiter.wait_for_marker(proc, "RHI: acceleration structures:", run_deadline, stderr_path)
                        time.sleep(0.3)
                        if not map_mode:
                            x11.send_key(win_id, proc.pid, "F8")
                            waiter.wait_for_marker(proc, f"QR_RESTART_{run_id}", run_deadline, stderr_path)
                            time.sleep(0.8)
                            x11.send_key(win_id, proc.pid, "F9")
                            waiter.wait_for_marker(proc, f"QR_RELOAD_{run_id}", run_deadline, stderr_path)
                            time.sleep(0.8)
                            if not test_stress_budget(run_deadline, minimum_capture_seconds):
                                print("The restart sequence consumed the capture budget; this attempt is deferred.")
                                x11.send_key(win_id, proc.pid, "F11")
                                proc.wait(timeout=max(1.0, get_stress_exit_timeout(run_deadline) / 1000.0))
                                break
                        else:
                            x11.send_key(win_id, proc.pid, "F9")
                            waiter.wait_for_marker(proc, f"QR_RELOAD_{run_id}", run_deadline, stderr_path)
                            time.sleep(0.3)

                        time.sleep(args.warmup)
                        x11.send_key(win_id, proc.pid, "F7")
                        waiter.wait_for_marker(proc, f"QR_EFFECTIVE_{run_id}", run_deadline, stderr_path)
                        if not x11.force_focus(win_id, proc.pid, retries=10, sleep_s=0.3):
                            raise RuntimeError("Runtime lacks foreground focus; refusing a contaminated measurement.")

                        _check_foreign_activity(proc.pid, ancestors, check_gpu=True)
                        x11.send_key(win_id, proc.pid, "F4")
                        waiter.wait_for_marker(proc, f"QR_START_{run_id}", run_deadline, stderr_path)

                        if args.move_during_capture:
                            x11.set_key_state(win_id, proc.pid, "d", True, require_focus=True)
                            move_key_held = True

                        if not test_stress_budget(run_deadline, args.seconds + shutdown_reserve):
                            print("Start marker consumed the capture budget; this attempt is deferred.")
                            if move_key_held:
                                x11.set_key_state(win_id, proc.pid, "d", False, require_focus=False)
                                move_key_held = False
                            x11.send_key(win_id, proc.pid, "F5")
                            x11.send_key(win_id, proc.pid, "F11")
                            proc.wait(timeout=max(1.0, get_stress_exit_timeout(run_deadline) / 1000.0))
                            break

                        cap_deadline = get_stress_deadline(run_deadline - shutdown_reserve, args.seconds)
                        next_owner_check = time.monotonic()
                        focus_lost_since: float | None = None
                        while time.monotonic() < cap_deadline:
                            if test_stop_requested():
                                break
                            if not x11.owns_focus(proc.pid):
                                if focus_lost_since is None:
                                    focus_lost_since = time.monotonic()
                                    focus_losses += 1
                                x11.force_focus(win_id, proc.pid, retries=2, sleep_s=0.1)
                                if (time.monotonic() - focus_lost_since) > 3.0:
                                    raise RuntimeError("Focus was lost during capture.")
                            else:
                                focus_lost_since = None

                            if time.monotonic() >= next_owner_check:
                                _check_foreign_activity(proc.pid, ancestors, check_gpu=False)
                                next_owner_check = time.monotonic() + 1.0
                            time.sleep(0.1)

                        _check_foreign_activity(proc.pid, ancestors, check_gpu=True)
                        if focus_losses != 0:
                            raise RuntimeError(f"Capture lost runtime focus {focus_losses} time(s); refusing a contaminated measurement.")

                        if move_key_held:
                            x11.set_key_state(win_id, proc.pid, "d", False, require_focus=False)
                            move_key_held = False

                        x11.send_key(win_id, proc.pid, "F5")
                        if not test_stop_requested():
                            waiter.wait_for_marker(proc, f"QR_STOP_{run_id}", run_deadline, stderr_path)
                        if args.screenshot and test_stress_budget(run_deadline, 10):
                            x11.send_key(win_id, proc.pid, "F6")
                            time.sleep(2.0)
                        x11.send_key(win_id, proc.pid, "F11")
                        exit_timeout_ms = get_stress_exit_timeout(run_deadline)
                        proc.wait(timeout=max(1.0, exit_timeout_ms / 1000.0))
                        if proc.returncode != 0:
                            raise RuntimeError(f"Runtime exit code {proc.returncode}")

                    if test_stop_requested():
                        raise RuntimeError("CANCELLED: owned runtime stopped; capture remains unaccepted.")

                    window_checks: dict[str, Any] | None = None
                    if args.stats_level > 0:
                        new_dumps = sorted(
                            (p for p in game_dir.glob("stats-*.dump") if p not in old_dumps),
                            key=lambda p: p.stat().st_mtime,
                        )
                        if not new_dumps:
                            raise RuntimeError("No stats dump was generated")
                        dump_src = new_dumps[-1]
                        shutil.copy2(dump_src, output / f"{label}.dump")
                        if args.stats_level == 3:
                            window_checks = validate_profiler_windows(dump_src)

                    if not bench_path.is_file():
                        raise RuntimeError(f"Missing benchmark log: {bench_path}")
                    all_bench_lines = bench_path.read_text(encoding="utf-8", errors="replace").splitlines()
                    new_block = all_bench_lines[old_bench_lines:]
                    (output / f"{label}.bench.log").write_text("\n".join(new_block) + "\n", encoding="utf-8")

                    settings_lines = [line for line in new_block if line.startswith("settings ")]
                    settings_line = settings_lines[-1] if settings_lines else ""
                    validate_settings_line(
                        settings_line=settings_line,
                        fsr=fsr,
                        rt_profile=args.rt_profile,
                        expected_vsync=args.expected_vsync,
                        expected_max_frame_latency=args.expected_max_frame_latency,
                        expected_restir=args.expected_restir,
                        allow_profile_overrides=args.allow_profile_overrides,
                    )

                    console_text = console_path.read_text(encoding="utf-8", errors="replace") if console_path.is_file() else ""
                    if map_mode:
                        expected_marker = f'"mapname" is "{expected_map}"'
                        if expected_marker not in console_text:
                            raise RuntimeError(f"Unexpected map in benchmark capture (expected {expected_marker})")
                    elif f"demo=maps/{expected_map}.bsp" not in "\n".join(new_block):
                        raise RuntimeError(f"Unexpected map in benchmark capture (expected maps/{expected_map}.bsp)")

                    sample_records = [line for line in new_block if line.startswith("frame_samples file=")]
                    if not sample_records:
                        raise RuntimeError("Per-frame benchmark samples are missing.")
                    match = re.fullmatch(r"frame_samples file=(\S+) samples=(\d+) dropped=0", sample_records[-1].strip())
                    if not match:
                        raise RuntimeError(
                            f"Per-frame benchmark samples are missing or truncated: {sample_records[-1]!r}"
                        )
                    sample_src = game_dir / match.group(1)
                    frame_path = output / f"{label}.frames.csv"
                    shutil.copy2(sample_src, frame_path)

                    if args.screenshot:
                        for image in sorted(game_dir.glob("screenshot*.png")):
                            if image not in old_screenshots:
                                shutil.copy2(image, output / f"{label}-{image.name}")

                    if console_path.is_file():
                        shutil.copy2(console_path, output / f"{label}.console.log")

                    manifest.setdefault("CaptureChecks", {})[label] = {
                        "FocusVerified": (not args.scheduled and focus_losses == 0),
                        "FocusLosses": focus_losses,
                        "ScheduledCommands": bool(args.scheduled),
                        "ScheduledWarmupFrames": scheduled_warmup_frames,
                        "ScheduledCaptureFrames": scheduled_capture_frames,
                        "Completed": True,
                        "StatsLevel": args.stats_level,
                    }
                    if window_checks is not None:
                        manifest["CaptureChecks"][label]["ProfilerWindows"] = window_checks
                    _write_manifest(manifest_path, manifest)

                    results.append(
                        {
                            "Save": save,
                            "Preset": args.preset,
                            "Repeat": str(repeat),
                            "Output": str(output),
                            "Frames": str(frame_path),
                            "Summary": str(output / f"{label}.summary.json"),
                        }
                    )
                    print(f"Saved {label}; runtime closed before analysis.")
                finally:
                    if move_key_held and win_id is not None and proc is not None:
                        try:
                            x11.set_key_state(win_id, proc.pid, "d", False, require_focus=False)
                        except Exception:
                            pass
                    kill_process_gracefully(proc)
                    if console_path.is_file():
                        shutil.copy2(console_path, output / f"{label}.console.log")
                    if saved_config is not None:
                        config_path.write_bytes(saved_config)
                    elif config_path.is_file():
                        config_path.unlink()
                    if fixture.is_file():
                        fixture.unlink()

    analyzer = PERF_DIR / "analyze_stress.py"
    for capture in results:
        proc = subprocess.run(
            [sys.executable, str(analyzer), capture["Frames"], "--json", capture["Summary"]],
            check=False,
        )
        if proc.returncode != 0:
            raise RuntimeError(f"Capture validation failed for {capture['Frames']}; see the frame samples.")

    for capture in results:
        print(
            f"PASS save={capture['Save']} preset={capture['Preset']} "
            f"repeat={capture['Repeat']} output={capture['Output']} summary={capture['Summary']}"
        )
    return results


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    run_stress(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
