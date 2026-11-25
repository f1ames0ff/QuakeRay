#!/usr/bin/env python3
# Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#

"""Linux menu UI-only stats & smoke runner (ports tests/perf/run_menu.ps1)."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
import re
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
    X11Controller,
    default_basedir,
    display_env,
    kill_process_gracefully,
)

UI_ZERO_GPU_PASSES = (
    "clouds",
    "sky",
    "primary",
    "decals",
    "godrays",
    "reflrefr",
    "reflgodr",
    "gradient",
    "direct",
    "indirect",
    "compose",
    "upscale",
    "post",
)
UI_ZERO_CPU_DRAW_PASSES = ("legacy_AS", "staging", "scene_record", "compose", "upscale", "post")
GPU_PASS_COL_RE = re.compile(r"^gpu\.(?!frame_ms).*_ms$")
STATS_DUMP_END_RE = re.compile(r"^rt_stats_dump_end(?:\s+\([^)]*\))?:\s+.*?\s+->\s+(.+\.dump)\s*$")


def median(values: list[float]) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    mid = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[mid]
    return (ordered[mid - 1] + ordered[mid]) / 2.0


def extract_values(rows: list[dict[str, str]], col: str) -> list[float]:
    out: list[float] = []
    for row in rows:
        raw = row.get(col)
        if raw is not None and raw != "":
            out.append(float(raw))
    return out


def parse_stats_dump_paths(console_text: str, base_dir: Path | None = None) -> list[Path]:
    paths: list[Path] = []
    for line in console_text.splitlines():
        match = STATS_DUMP_END_RE.match(line.strip())
        if match:
            p = Path(match.group(1).strip())
            if not p.is_absolute() and base_dir is not None:
                p = base_dir / p
            paths.append(p)
    return paths


def read_dump_rows(capture_path: Path) -> list[dict[str, str]]:
    if not capture_path.is_file():
        raise FileNotFoundError(f"Stats capture does not exist: {capture_path}")
    lines = [
        line
        for line in capture_path.read_text(encoding="utf-8", errors="replace").splitlines()
        if line and not line.startswith("#")
    ]
    rows = list(csv.DictReader(lines))
    if len(rows) < 5:
        raise ValueError(f"Too few samples in {capture_path} ({len(rows)} < 5)")
    return rows


def read_capture(game_dir: Path, previous_files: set[Path]) -> tuple[Path, list[dict[str, str]]]:
    candidates = sorted(
        (p for p in game_dir.glob("stats-*.dump") if p not in previous_files),
        key=lambda p: p.stat().st_mtime,
    )
    if not candidates:
        raise RuntimeError(f"No stats capture was written in {game_dir}")
    capture_path = candidates[-1]
    return capture_path, read_dump_rows(capture_path)


def check_ui_capture(rows: list[dict[str, str]], path: str | Path = "<memory>") -> None:
    for p in UI_ZERO_GPU_PASSES:
        vals = extract_values(rows, f"gpu.{p}_ms")
        if any(v != 0.0 for v in vals):
            raise ValueError(f"A UI-only frame ran the {p} pass: {path}")

    for row in rows:
        total_raw = row.get("gpu.frame_ms", "")
        if total_raw == "":
            continue
        total = float(total_raw)
        for key, raw_val in row.items():
            if raw_val and GPU_PASS_COL_RE.match(key):
                if float(raw_val) > total + 0.02:
                    raise ValueError(f"GPU pass exceeds its frame total: {key}")

    for p in UI_ZERO_CPU_DRAW_PASSES:
        vals = extract_values(rows, f"cpu.draw.{p}_max_ms")
        if any(v != 0.0 for v in vals):
            raise ValueError(f"A UI-only frame recorded {p} CPU work: {path}")


def _wait_lines(count: int) -> list[str]:
    lines: list[str] = []
    remaining = count
    while remaining > 0:
        chunk = min(60, remaining)
        lines.append("wait;" * chunk)
        remaining -= chunk
    return lines


def _build_scripted_cfg(tag: str, seconds: int, run_smoke: bool) -> str:
    ui_waits = max(200, seconds * 72)
    lines = [
        "cl_startdemos 0",
        "stopdemo",
        "disconnect",
        "vid_vsync 3",
        "host_maxfps 0",
        "rt_stats 3",
        "rt_stats_interval 0.2",
        "rt_ef_crt 0",
        "menu_main",
        f"echo QR_MENU_READY_{tag}",
        *_wait_lines(150),
        "rt_stats_dump_start",
        *_wait_lines(ui_waits),
        "rt_stats_dump_end",
        *_wait_lines(30),
        f"echo QR_MENU_UI1_{tag}",
        "screenshot",
        *_wait_lines(10),
    ]
    if run_smoke:
        lines.extend(
            [
                "rt_stats 0",
                *_wait_lines(10),
                "screenshot",
                *_wait_lines(10),
                "rt_ef_crt 1",
                *_wait_lines(10),
                "screenshot",
                *_wait_lines(10),
                "rt_ef_crt 0; rt_stats 3; map start",
                *_wait_lines(2),
                "cmd pext",
                *_wait_lines(180),
                "screenshot",
                *_wait_lines(10),
                "menu_main",
                *_wait_lines(30),
                "rt_stats_dump_start",
                *_wait_lines(240),
                "rt_stats_dump_end",
                *_wait_lines(30),
                f"echo QR_MENU_INGAME_{tag}",
                "screenshot",
                *_wait_lines(10),
                "disconnect; menu_main",
                *_wait_lines(120),
                "rt_stats_dump_start",
                *_wait_lines(240),
                "rt_stats_dump_end",
                *_wait_lines(30),
                f"echo QR_MENU_UI2_{tag}",
                "vid_unlock; vid_width 1280; vid_height 720; vid_restart",
                *_wait_lines(60),
                "screenshot",
                *_wait_lines(10),
            ]
        )
    lines.append("menu_main; toggleconsole; quit")
    return "\n".join(lines) + "\n"


def _build_interactive_cfg(tag: str) -> str:
    return "\n".join(
        [
            "cl_startdemos 0",
            "stopdemo",
            "disconnect",
            "vid_vsync 3",
            "host_maxfps 0",
            "rt_stats 3",
            "rt_stats_interval 0.2",
            "rt_ef_crt 0",
            'bind F4 "rt_stats_dump_start"',
            'bind F5 "rt_stats_dump_end"',
            'bind F6 "screenshot"',
            'bind F7 "rt_stats 0"',
            'bind F8 "rt_ef_crt 1"',
            'bind F9 "rt_ef_crt 0; rt_stats 3; map start"',
            'bind F10 "disconnect; menu_main"',
            'bind F11 "menu_main; toggleconsole; quit"',
            'bind F12 "vid_unlock; vid_width 1280; vid_height 720; vid_restart"',
            "menu_main",
            f"echo QR_MENU_READY_{tag}",
        ]
    ) + "\n"


def _run_arm(
    directory: Path,
    name: str,
    *,
    seconds: int,
    width: int,
    height: int,
    run_smoke: bool,
    validation: bool,
    interactive: bool,
) -> dict[str, Any]:
    directory = directory.resolve()
    exe = directory / "quakeray"
    if not exe.is_file():
        raise FileNotFoundError(f"Missing Linux runtime binary: {exe}")

    game_dir = directory / "id1"
    if not (game_dir / "pak0.pak").is_file() and not (game_dir / "PAK0.PAK").is_file():
        raise FileNotFoundError(f"Missing base game data pak0.pak in {game_dir}")

    config_path = game_dir / "config.cfg"
    library_config = directory / "qray.txt"
    saved_config = config_path.read_bytes() if config_path.is_file() else None
    saved_library_config = library_config.read_bytes() if library_config.is_file() else None

    tag = uuid.uuid4().hex[:8]
    fixture = game_dir / f"qr_m_{tag}.cfg"
    console = directory / "qconsole.log"
    stderr_path = directory / f"menu-{name}-{tag}.stderr.log"
    stdout_path = directory / f"menu-{name}-{tag}.stdout.log"
    previous = set(game_dir.glob("stats-*.dump"))
    proc: subprocess.Popen[bytes] | None = None

    fixture.write_text(
        _build_interactive_cfg(tag) if interactive else _build_scripted_cfg(tag, seconds, run_smoke),
        encoding="ascii",
    )

    try:
        if validation:
            library_config.write_text("vulkanvalidation\n", encoding="ascii")
        if console.is_file():
            console.unlink()

        cmd = [
            str(exe),
            "-basedir",
            ".",
            "-window",
            "-width",
            str(width),
            "-height",
            str(height),
            "-nosound",
            "-condebug",
            "+exec",
            f"qr_m_{tag}.cfg",
        ]
        with stdout_path.open("wb") as out_f, stderr_path.open("wb") as err_f:
            proc = subprocess.Popen(
                cmd,
                cwd=str(directory),
                env=display_env(),
                stdout=out_f,
                stderr=err_f,
            )

        with X11Controller() as x11:
            waiter = ConsoleWaiter(console)
            deadline = time.monotonic() + 180.0
            if interactive:
                win_id = x11.wait_for_window(proc, timeout_s=120.0)
                waiter.wait_for_marker(proc, f"QR_MENU_READY_{tag}", deadline, stderr_path)
                if not x11.force_focus(win_id, proc.pid, retries=10, sleep_s=0.3):
                    raise RuntimeError(f"{name} window could not acquire focus")

                time.sleep(5.0)
                x11.send_key(win_id, proc.pid, "F4")
                time.sleep(seconds)
                x11.send_key(win_id, proc.pid, "F5")
                time.sleep(1.0)
                cap_path, cap_rows = read_capture(game_dir, previous)
                if name == "candidate":
                    check_ui_capture(cap_rows, cap_path)

                x11.send_key(win_id, proc.pid, "F6")

                if run_smoke:
                    x11.send_key(win_id, proc.pid, "F7")
                    x11.send_key(win_id, proc.pid, "F6")
                    x11.send_key(win_id, proc.pid, "F8")
                    x11.send_key(win_id, proc.pid, "F6")
                    x11.send_key(win_id, proc.pid, "F9")
                    time.sleep(5.0)
                    x11.send_key(win_id, proc.pid, "F6")
                    x11.send_key(win_id, proc.pid, "Escape")

                    before_pause = set(game_dir.glob("stats-*.dump"))
                    x11.send_key(win_id, proc.pid, "F4")
                    time.sleep(3.0)
                    x11.send_key(win_id, proc.pid, "F5")
                    time.sleep(1.0)
                    _, pause_rows = read_capture(game_dir, before_pause)
                    primary_med = median(extract_values(pause_rows, "gpu.primary_ms"))
                    if primary_med is None or primary_med <= 0.0:
                        raise RuntimeError("An in-game menu stopped primary rendering")

                    x11.send_key(win_id, proc.pid, "F6")
                    x11.send_key(win_id, proc.pid, "F10")
                    time.sleep(3.0)

                    before_return = set(game_dir.glob("stats-*.dump"))
                    x11.send_key(win_id, proc.pid, "F4")
                    time.sleep(3.0)
                    x11.send_key(win_id, proc.pid, "F5")
                    time.sleep(1.0)
                    ret_path, ret_rows = read_capture(game_dir, before_return)
                    if name == "candidate":
                        check_ui_capture(ret_rows, ret_path)

                    x11.send_key(win_id, proc.pid, "F12")
                    time.sleep(2.0)
                    x11.send_key(win_id, proc.pid, "F6")

                x11.send_key(win_id, proc.pid, "F11")
                proc.wait(timeout=15.0)
                if proc.returncode != 0:
                    raise RuntimeError(f"{name} exited with code {proc.returncode}")
            else:
                waiter.wait_for_marker(proc, f"QR_MENU_READY_{tag}", deadline, stderr_path)
                win_id = x11.find_window_for_pid(proc.pid)
                if win_id:
                    x11.force_focus(win_id, proc.pid, retries=3, sleep_s=0.15)
                remaining = max(5.0, deadline - time.monotonic())
                proc.wait(timeout=remaining)
                if proc.returncode != 0:
                    raise RuntimeError(f"{name} exited with code {proc.returncode}; see {console} and {stderr_path}")
                if not console.is_file():
                    raise RuntimeError(f"Missing console log: {console}")
                console_text = console.read_text(encoding="utf-8", errors="replace")
                dump_paths = parse_stats_dump_paths(console_text, base_dir=directory)
                expected_dumps = 3 if run_smoke else 1
                if len(dump_paths) < expected_dumps:
                    raise RuntimeError(f"Expected {expected_dumps} stats dumps in console log, found {len(dump_paths)}")
                cap_path = dump_paths[0]
                cap_rows = read_dump_rows(cap_path)
                if name == "candidate":
                    check_ui_capture(cap_rows, cap_path)
                if run_smoke:
                    pause_rows = read_dump_rows(dump_paths[1])
                    primary_med = median(extract_values(pause_rows, "gpu.primary_ms"))
                    if primary_med is None or primary_med <= 0.0:
                        raise RuntimeError("An in-game menu stopped primary rendering")
                    ret_path = dump_paths[2]
                    ret_rows = read_dump_rows(ret_path)
                    if name == "candidate":
                        check_ui_capture(ret_rows, ret_path)

        result: dict[str, Any] = {
            "Arm": name,
            "Mode": "interactive" if interactive else "scripted",
            "DrawFrameWindowMaxMedianMs": median(extract_values(cap_rows, "cpu.qrDrawFrame_ms")),
            "DrawFrameAverageMedianMs": median(extract_values(cap_rows, "cpu.qrDrawFrame_avg_ms")),
            "GpuMedianMs": median(extract_values(cap_rows, "gpu.frame_ms")),
            "RasterMedian": median(extract_values(cap_rows, "calls_raster")),
            "Capture": str(cap_path),
            "ValidationEnabled": bool(validation),
            "ValidationErrorCount": None,
            "ProcessExitCode": proc.returncode,
        }
        if validation and console.is_file():
            console_text = console.read_text(encoding="utf-8", errors="replace")
            matches = re.findall(r"VUID-|Validation Error|NVRHI::ERROR", console_text)
            result["ValidationErrorCount"] = len(matches)
            if matches:
                raise RuntimeError(f"Renderer validation errors ({len(matches)}); see {console}")

        result_path = stdout_path.with_suffix("").with_suffix(".result.json")
        result_path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        return result
    finally:
        kill_process_gracefully(proc)
        if saved_config is not None:
            config_path.write_bytes(saved_config)
        elif config_path.is_file():
            config_path.unlink()
        if validation:
            if saved_library_config is not None:
                library_config.write_bytes(saved_library_config)
            elif library_config.is_file():
                library_config.unlink()
        if fixture.is_file():
            fixture.unlink()


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run QuakeRay Linux menu UI-only performance & smoke test.")
    parser.add_argument("--candidate", default="", help="Candidate runtime directory (default: autodetect)")
    parser.add_argument("--baseline", default="", help="Optional baseline runtime directory for A/B")
    parser.add_argument("--seconds", type=int, default=8)
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--validation", action="store_true")
    parser.add_argument("--baseline-only", action="store_true")
    parser.add_argument("--interactive", action="store_true", help="Use XTest interactive keystrokes instead of scripted mode")
    args = parser.parse_args(argv)
    if not (2 <= args.seconds <= 25):
        parser.error("--seconds must be in 2..25")
    if args.baseline_only and not args.baseline:
        parser.error("--baseline-only requires --baseline")
    return args


def run_menu(args: argparse.Namespace) -> list[dict[str, Any]]:
    candidate = Path(args.candidate).resolve() if args.candidate else default_basedir().resolve()
    results: list[dict[str, Any]] = []
    with MachineGuard():
        if args.baseline:
            results.append(
                _run_arm(
                    Path(args.baseline),
                    "baseline",
                    seconds=args.seconds,
                    width=args.width,
                    height=args.height,
                    run_smoke=bool(args.baseline_only and args.smoke),
                    validation=args.validation,
                    interactive=args.interactive,
                )
            )
        if not args.baseline_only:
            results.append(
                _run_arm(
                    candidate,
                    "candidate",
                    seconds=args.seconds,
                    width=args.width,
                    height=args.height,
                    run_smoke=bool(args.smoke),
                    validation=args.validation,
                    interactive=args.interactive,
                )
            )
    print(json.dumps(results, indent=2))
    return results


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    run_menu(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
