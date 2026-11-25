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

"""Linux worker adapter for QuakeRay MCP jobs (counterpart to worker.ps1)."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

QUAKERAY_ROOT = Path(__file__).resolve().parents[2]
PERF_DIR = QUAKERAY_ROOT / "tests/perf"
if str(PERF_DIR) not in sys.path:
    sys.path.insert(0, str(PERF_DIR))

from machine_guard import MachineGuard, sha256_file  # noqa: E402


MAX_SANDBOX_BYTES = 8 * 1024 * 1024 * 1024
SKIP_TOP_LEVEL_RE = re.compile(r"^(audit-|benchmark|qconsole|crash)")
SKIP_TOP_LEVEL_EXACT = frozenset({"qray_stage", "renderer"})


def _copy_sandbox_runtime(src: Path, dst: Path) -> None:
    if not src.is_dir():
        raise RuntimeError(f"ENV_MISSING_BUILD: runtime directory not found: {src}")
    total_bytes = 0
    for item in src.rglob("*"):
        if item.is_symlink():
            raise RuntimeError("PERMISSION_DENIED: runtime contains symlinks; prepare an independent runtime.")
        if item.is_file():
            total_bytes += item.stat().st_size
            if total_bytes > MAX_SANDBOX_BYTES:
                raise RuntimeError("DISK_QUOTA: runtime exceeds the 8 GiB sandbox limit.")

    dst.mkdir(parents=True, exist_ok=True)
    for child in src.iterdir():
        if SKIP_TOP_LEVEL_RE.match(child.name) or child.name in SKIP_TOP_LEVEL_EXACT:
            continue
        target = dst / child.name
        if child.is_dir():
            shutil.copytree(child, target, dirs_exist_ok=True)
        else:
            shutil.copy2(child, target)


def _snapshot_files(root: Path) -> set[Path]:
    return {p.resolve() for p in root.rglob("*") if p.is_file()}


def _git_head(source: Path) -> str:
    proc = subprocess.run(["git", "-C", str(source), "rev-parse", "HEAD"], capture_output=True, text=True, check=True)
    return proc.stdout.strip()


def run_worker(spec_path: Path) -> None:
    request = json.loads(spec_path.read_text(encoding="utf-8"))
    job_dir = spec_path.resolve().parent
    os.environ["QUAKERAY_JOB_STOP_FILE"] = str(job_dir / "stop.request")

    operation = request["operation"]
    guard = MachineGuard()
    guard.__enter__()
    try:
        if operation == "build":
            source = Path(request.get("source_root") or request["repo_root"]).resolve()
            runtime = Path(request["runtime"]).resolve()
            env = os.environ.copy()
            env["QR_BUILD_DIR"] = str(runtime)
            env["QR_BUILD_TESTS"] = "1" if request.get("tests") else "0"
            proc = subprocess.run(["bash", str(source / "build_linux.sh")], cwd=str(source), env=env, check=False)
            if proc.returncode != 0:
                raise RuntimeError(f"BUILD_FAILED: exit code {proc.returncode}")
            exe = runtime / "quakeray"
            pkz = runtime / "id1/qray.pkz"
            if not exe.is_file() or not pkz.is_file():
                raise RuntimeError("BUILD_FAILED: deployed runtime is incomplete.")
            receipt = {
                "SchemaVersion": 1,
                "Producer": "mcp-worker-debug-build",
                "BuildConfig": "Debug",
                "Revision": _git_head(source),
                "ExecutableSha256": sha256_file(exe),
                "EngineAssetsSha256": sha256_file(pkz),
            }
            (runtime / "qray-build.json").write_text(json.dumps(receipt), encoding="utf-8")
            (job_dir / "result.json").write_text(
                json.dumps({"operation": "build", "runtime": str(runtime), "tests": "not_run"}),
                encoding="utf-8",
            )
            return

        sandbox = job_dir / "runtime"
        _copy_sandbox_runtime(Path(request["runtime"]).resolve(), sandbox)
        before = _snapshot_files(sandbox)

        deadline = datetime.fromisoformat(request["deadline_utc"].replace("Z", "+00:00"))
        remaining = int((deadline.astimezone(timezone.utc) - datetime.now(timezone.utc)).total_seconds())
        if remaining < 30:
            raise RuntimeError("RUN_TIMEOUT: runtime preparation consumed the capture budget.")

        baseline_dir: Path | None = None
        if operation in ("menu", "suite") and request.get("baseline"):
            baseline_dir = job_dir / "baseline"
            _copy_sandbox_runtime(Path(request["baseline"]).resolve(), baseline_dir)
            before |= _snapshot_files(baseline_dir)

        if operation == "suite":
            asset_source = Path(request["asset_source"]).resolve()
            shutil.copytree(asset_source / "ad", sandbox / "ad", dirs_exist_ok=True)
            for pak in ("pak0.pak", "pak1.pak"):
                src_pak = asset_source / "id1" / pak
                if src_pak.is_file():
                    shutil.copy2(src_pak, sandbox / "id1" / pak)
            before = _snapshot_files(job_dir)

        # Release the parent flock before launching runner subprocesses so they can acquire MachineGuard.
        guard.__exit__(None, None, None)
        guard = None

        if operation == "menu":
            cmd = [
                sys.executable,
                "-B",
                str(PERF_DIR / "run_menu.py"),
                "--candidate",
                str(sandbox),
                "--seconds",
                str(request["seconds"]),
            ]
            if baseline_dir is not None:
                cmd.extend(["--baseline", str(baseline_dir)])
            if request.get("smoke"):
                cmd.append("--smoke")
            if request.get("validation"):
                cmd.append("--validation")
            rc = subprocess.run(cmd, cwd=str(QUAKERAY_ROOT), check=False).returncode
            if rc != 0:
                raise RuntimeError(f"RUN_FAILED: run_menu.py exited with {rc}")
        elif operation == "suite":
            if baseline_dir is None:
                raise RuntimeError("INVALID_ARGUMENT: suite requires a baseline runtime")
            arms = [sandbox, baseline_dir] if request.get("order") == "candidate_first" else [baseline_dir, sandbox]
            for arm in arms:
                cmd = [
                    sys.executable,
                    "-B",
                    str(PERF_DIR / "run_stress.py"),
                    "--basedir",
                    str(arm),
                    "--save",
                    str(request["save"]),
                    "--preset",
                    str(request["preset"]),
                    "--seconds",
                    str(request["seconds"]),
                    "--warmup",
                    str(request["warmup"]),
                    "--repeats",
                    "1",
                    "--max-run-seconds",
                    "300",
                    "--stats-level",
                    "0",
                    "--tag",
                    "experiment",
                ]
                if subprocess.run(cmd, cwd=str(QUAKERAY_ROOT), check=False).returncode != 0:
                    raise RuntimeError("RUN_FAILED: experiment arm failed.")
        elif operation in ("benchmark", "capture"):
            cmd = [
                sys.executable,
                "-B",
                str(PERF_DIR / "run_stress.py"),
                "--basedir",
                str(sandbox),
                "--save",
                str(request["save"]),
                "--preset",
                str(request["preset"]),
                "--seconds",
                str(request["seconds"]),
                "--warmup",
                str(request["warmup"]),
                "--repeats",
                str(request["repeats"]),
                "--max-run-seconds",
                str(min(300, remaining)),
                "--stats-level",
                str(request["stats_level"]),
                "--tag",
                "mcp",
            ]
            if subprocess.run(cmd, cwd=str(QUAKERAY_ROOT), check=False).returncode != 0:
                raise RuntimeError("RUN_FAILED: run_stress.py did not complete.")
        else:
            raise RuntimeError("INVALID_ARGUMENT: unknown worker operation.")

        captures = sorted(
            str(p)
            for p in _snapshot_files(job_dir) - before
            if p.name.endswith(".frames.csv") or (p.name.startswith("stats-") and p.name.endswith(".dump")) or p.name.endswith(".dump")
        )
        if not captures:
            raise RuntimeError("CAPTURE_MISSING: runner produced no complete capture.")
        (job_dir / "result.json").write_text(
            json.dumps({"operation": operation, "runtime": str(sandbox), "captures": captures}),
            encoding="utf-8",
        )
    finally:
        if guard is not None:
            guard.__exit__(None, None, None)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Execute one QuakeRay MCP worker job on Linux.")
    parser.add_argument("--spec", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        run_worker(args.spec)
        return 0
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
