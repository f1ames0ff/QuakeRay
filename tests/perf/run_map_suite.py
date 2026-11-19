#!/usr/bin/env python3
"""Linux multi-map stress suite orchestrator (ports tests/perf/run_map_suite.ps1)."""

from __future__ import annotations

import argparse
from datetime import datetime
import json
from pathlib import Path
import re
import subprocess
import sys
from typing import Any

PERF_DIR = Path(__file__).resolve().parent
if str(PERF_DIR) not in sys.path:
    sys.path.insert(0, str(PERF_DIR))

from machine_guard import default_basedir, stage_ad_assets  # noqa: E402


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run QuakeRay Linux stress suite across one or more maps.")
    parser.add_argument("--game", choices=("id1", "mg1", "mg3", "ad"), default="ad")
    parser.add_argument(
        "--maps",
        action="append",
        default=[],
        help="Map name(s), comma-separated or repeatable (omit to enumerate all maps in --game)",
    )
    parser.add_argument("--basedir", default="", help="Runtime build directory containing quakeray")
    parser.add_argument("--ad-dir", default="", help="Source Arcane Dimensions directory")
    parser.add_argument("--ad-saves-dir", default="", help="Source directory containing canonical AD .sav files")
    parser.add_argument("--preset", choices=("balanced", "quality", "performance", "ultra"), default="balanced")
    parser.add_argument("--rt-profile", choices=("audit", "medium"), default="medium")
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--seconds", type=int, default=10)
    parser.add_argument("--repeats", type=int, default=1)
    parser.add_argument("--run-seconds", type=int, default=300)
    parser.add_argument("--stats-level", type=int, choices=(0, 1, 2, 3), default=3)
    parser.add_argument("--fullscreen", type=int, choices=(0, 1, 2), default=2)
    parser.add_argument("--expected-vsync", type=int, choices=(1, 2, 3), default=1)
    parser.add_argument("--expected-max-frame-latency", type=int, choices=(1,), default=1)
    parser.add_argument("--expected-restir", type=int, choices=(0, 1), default=0)
    parser.add_argument("--override", action="append", default=[])
    parser.add_argument("--tag", default="map-suite")
    parser.add_argument("--no-sound", action="store_true")
    parser.add_argument("--scheduled", action="store_true")
    parser.add_argument("--move-during-capture", action="store_true")
    parser.add_argument("--allow-profile-overrides", action="store_true")
    parser.add_argument("--continue-on-error", action="store_true")
    args = parser.parse_args(argv)
    if not (2 <= args.warmup <= 30):
        parser.error("--warmup must be in 2..30")
    if not (2 <= args.seconds <= 25):
        parser.error("--seconds must be in 2..25")
    if not (1 <= args.repeats <= 2):
        parser.error("--repeats must be in 1..2")
    if not (30 <= args.run_seconds <= 300):
        parser.error("--run-seconds must be in 30..300")
    return args


def resolve_maps(basedir: Path, game: str, raw_maps: list[str]) -> list[str]:
    selected: list[str] = []
    for entry in raw_maps:
        for item in entry.split(","):
            name = item.strip()
            if name:
                if not re.fullmatch(r"^[A-Za-z0-9_./-]+$", name):
                    raise ValueError(f"Invalid map name: {name!r}")
                selected.append(name)
    if selected:
        return selected

    enum_script = PERF_DIR / "enumerate_game_maps.py"
    proc = subprocess.run(
        [sys.executable, str(enum_script), "--data-root", str(basedir), "--game", game],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"Map enumeration failed: {proc.stderr.strip() or proc.stdout.strip()}")
    payload = json.loads(proc.stdout)
    maps = [m["name"] for m in payload.get("maps", []) if m.get("name")]
    if not maps:
        raise RuntimeError(f"No maps enumerated in {basedir / game}")
    return maps


def run_map_suite(args: argparse.Namespace) -> tuple[Path, dict[str, Any]]:
    basedir = Path(args.basedir).resolve() if args.basedir else default_basedir().resolve()
    if args.game == "ad" or args.ad_dir or args.ad_saves_dir:
        stage_ad_assets(
            basedir,
            ad_dir=args.ad_dir or None,
            saves_dir=args.ad_saves_dir or None,
        )

    maps = resolve_maps(basedir, args.game, args.maps)
    stress_runner = PERF_DIR / "run_stress.py"
    results: list[dict[str, Any]] = []
    failed_count = 0

    for map_name in maps:
        safe_map = re.sub(r"[^A-Za-z0-9_.-]", "_", map_name)
        map_tag = f"{args.tag}-{args.game}-{safe_map}"
        safe_prefix = re.sub(r"[^a-zA-Z0-9_-]", "_", map_tag)
        before_dirs = set(basedir.glob(f"audit-{safe_prefix}-*"))

        cmd = [
            sys.executable,
            str(stress_runner),
            "--game",
            args.game,
            "--map",
            map_name,
            "--preset",
            args.preset,
            "--rt-profile",
            args.rt_profile,
            "--basedir",
            str(basedir),
            "--warmup",
            str(args.warmup),
            "--seconds",
            str(args.seconds),
            "--repeats",
            str(args.repeats),
            "--max-run-seconds",
            str(args.run_seconds),
            "--stats-level",
            str(args.stats_level),
            "--fullscreen",
            str(args.fullscreen),
            "--expected-vsync",
            str(args.expected_vsync),
            "--expected-max-frame-latency",
            str(args.expected_max_frame_latency),
            "--expected-restir",
            str(args.expected_restir),
            "--tag",
            map_tag,
        ]
        if args.no_sound:
            cmd.append("--no-sound")
        if args.scheduled:
            cmd.append("--scheduled")
        if args.move_during_capture:
            cmd.append("--move-during-capture")
        if args.allow_profile_overrides:
            cmd.append("--allow-profile-overrides")
        for ovr in args.override:
            cmd.extend(["--override", ovr])

        try:
            proc = subprocess.run(cmd, check=False)
            if proc.returncode != 0:
                raise RuntimeError(f"run_stress.py exited with code {proc.returncode}")

            new_dirs = sorted(
                (p for p in basedir.glob(f"audit-{safe_prefix}-*") if p.is_dir() and p not in before_dirs),
                key=lambda p: p.stat().st_mtime,
            )
            if not new_dirs:
                all_matching = sorted(
                    (p for p in basedir.glob(f"audit-{safe_prefix}-*") if p.is_dir()),
                    key=lambda p: p.stat().st_mtime,
                )
                capture_dir = all_matching[-1] if all_matching else None
            else:
                capture_dir = new_dirs[-1]

            summary_files = sorted(capture_dir.glob("*.summary.json")) if capture_dir else []
            manifest_file = (capture_dir / "manifest.json") if capture_dir else None
            results.append(
                {
                    "Game": args.game,
                    "Map": map_name,
                    "Status": "passed",
                    "Capture": str(capture_dir) if capture_dir else None,
                    "Manifest": str(manifest_file) if manifest_file and manifest_file.is_file() else None,
                    "Summary": str(summary_files[0]) if summary_files else None,
                }
            )
        except Exception as exc:
            failed_count += 1
            results.append(
                {
                    "Game": args.game,
                    "Map": map_name,
                    "Status": "failed",
                    "Error": str(exc),
                }
            )
            if not args.continue_on_error:
                break

    suite_tag = f"{re.sub(r'[^A-Za-z0-9_-]', '_', args.tag)}-{args.game}-{datetime.now().strftime('%Y%m%d-%H%M%S')}"
    result_path = basedir / f"audit-{suite_tag}.suite.json"
    payload: dict[str, Any] = {
        "Timestamp": datetime.now().astimezone().isoformat(),
        "Game": args.game,
        "Tag": args.tag,
        "Preset": args.preset,
        "RtProfile": args.rt_profile,
        "Total": len(results),
        "Passed": sum(1 for r in results if r["Status"] == "passed"),
        "Failed": failed_count,
        "Results": results,
    }
    result_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    for row in results:
        detail = row.get("Summary") or row.get("Error") or ""
        print(f"[{row['Status']}] {row['Game']}/{row['Map']} {detail}")
    print(f"Suite result: {result_path}")
    return result_path, payload


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    _, payload = run_map_suite(args)
    return 1 if payload["Failed"] > 0 else 0


if __name__ == "__main__":
    raise SystemExit(main())
