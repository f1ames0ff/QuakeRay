#!/usr/bin/env python3
"""Linux fixed-scene cluster sampling benchmark runner (ports tests/perf/run_place.ps1)."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
from typing import Any

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
    stage_ad_assets,
)

PLACE_SLOTS = (
    "frame",
    "clusters",
    "clust lists",
    "clust resolve",
    "clust vis",
    "clust topup",
    "clust fill",
    "clust tail",
    "clust upload",
    "clust publish",
)
BENCH_HEADER_RE = re.compile(
    r"^#\s*rt_bench\s+(.*?)\s+demo=(\S+)\s+frames=(\d+)\s+seconds=([0-9.]+)\s+fps=([0-9.]+)\s+interrupted=(\d+)"
)
SAMPLING_RE = re.compile(r"^settings\s+.*?rt_cluster_sampling=(\d+)")
SLOT_RE = re.compile(r"^cpu\.slot\s+(.+?)\s+avg_ms=([0-9.]+)\s+max_ms=([0-9.]+)")
CLUSTER_LISTS_RE = re.compile(
    r"^cpu\.cluster\s+lists\s+hits=(\d+)\s+misses=(\d+)\s+set=(\d+)\s+move=(\d+)\s+other=(\d+)"
)


def parse_place_blocks(lines: list[str]) -> list[dict[str, Any]]:
    blocks: list[dict[str, Any]] = []
    current: dict[str, Any] | None = None
    for line in lines:
        header = BENCH_HEADER_RE.match(line)
        if header:
            current = {
                "demo": header.group(2),
                "frames": int(header.group(3)),
                "seconds": float(header.group(4)),
                "fps": float(header.group(5)),
                "interrupted": int(header.group(6)),
                "sampling": None,
                "slots": {},
                "cluster_lists": None,
            }
            blocks.append(current)
            continue
        if current is None:
            continue
        sampling = SAMPLING_RE.match(line)
        if sampling:
            current["sampling"] = int(sampling.group(1))
            continue
        slot = SLOT_RE.match(line)
        if slot:
            slot_name = slot.group(1).strip()
            if slot_name in PLACE_SLOTS:
                current["slots"][slot_name] = (slot.group(2), slot.group(3))
            continue
        lists = CLUSTER_LISTS_RE.match(line)
        if lists:
            current["cluster_lists"] = {
                "hits": int(lists.group(1)),
                "misses": int(lists.group(2)),
                "set": int(lists.group(3)),
                "move": int(lists.group(4)),
                "other": int(lists.group(5)),
            }
    return blocks


def format_place_blocks(blocks: list[dict[str, Any]]) -> str:
    out: list[str] = []
    for block in blocks:
        out.append("")
        out.append(
            f"== demo={block['demo']} frames={block['frames']} seconds={block['seconds']} "
            f"fps={block['fps']} interrupted={block['interrupted']}"
        )
        if block["sampling"] is not None:
            out.append(f"   sampling={block['sampling']}")
        for slot_name, (avg_ms, max_ms) in block["slots"].items():
            out.append(f"   {slot_name:<12} avg={avg_ms:>8} max={max_ms:>8}")
        if block["cluster_lists"]:
            cl = block["cluster_lists"]
            out.append(
                f"   hits={cl['hits']} misses={cl['misses']} set={cl['set']} move={cl['move']} other={cl['other']}"
            )
    return "\n".join(out)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run QuakeRay Linux fixed-scene placement benchmark.")
    parser.add_argument("--save", default="qr_ad_swamp", help="AD save name (default: qr_ad_swamp)")
    parser.add_argument("--mode", choices=("gpumax", "cpumax", "both"), default="both")
    parser.add_argument("--seconds", type=int, default=12)
    parser.add_argument("--warmup", type=int, default=4)
    parser.add_argument("--basedir", default="", help="Runtime build directory containing quakeray")
    parser.add_argument("--ad-dir", default="", help="Source Arcane Dimensions directory")
    parser.add_argument("--ad-saves-dir", default="", help="Source directory containing canonical AD .sav files")
    parser.add_argument("--tag", default="", help="Output prefix tag (writes perf-<tag>-<mode>.log)")
    parser.add_argument("--no-sound", action="store_true")
    args = parser.parse_args(argv)
    if args.seconds < 1 or args.warmup < 0:
        parser.error("--seconds must be >= 1 and --warmup >= 0")
    return args


def _send_key_and_wait_marker(
    x11: X11Controller,
    waiter: ConsoleWaiter,
    proc: subprocess.Popen[bytes],
    win_id: str,
    key_name: str,
    marker: str,
    timeout_s: float,
    stderr_file: Path,
    *,
    ack_marker: str | None = None,
    retry_after_s: float = 4.0,
    max_attempts: int = 3,
) -> str:
    """Send a bound X11 key and wait for its console marker, retrying on dropped input."""
    before_marker = waiter.count_marker(marker)
    before_ack = waiter.count_marker(ack_marker) if ack_marker is not None else 0
    overall_deadline = time.monotonic() + timeout_s

    for attempt in range(1, max_attempts + 1):
        win_id = x11.find_window_for_pid(proc.pid) or win_id
        x11.force_focus(win_id, proc.pid, retries=5, sleep_s=0.2)
        x11.send_key(win_id, proc.pid, key_name)

        attempt_deadline = (
            overall_deadline
            if attempt == max_attempts
            else min(overall_deadline, time.monotonic() + retry_after_s)
        )
        while True:
            if waiter.count_marker(marker) >= before_marker + 1:
                return win_id
            if ack_marker is not None and waiter.count_marker(ack_marker) >= before_ack + 1:
                waiter.wait_for_marker(
                    proc,
                    marker,
                    overall_deadline,
                    stderr_file,
                    min_count=before_marker + 1,
                )
                return win_id
            if proc.poll() is not None:
                raise RuntimeError(f"Runtime exited with code {proc.returncode} before {marker}")
            now = time.monotonic()
            if now >= overall_deadline or now >= attempt_deadline:
                break
            time.sleep(0.1)

        if time.monotonic() >= overall_deadline:
            break

    waiter.wait_for_marker(
        proc,
        marker,
        overall_deadline,
        stderr_file,
        min_count=before_marker + 1,
    )
    return win_id


def run_place(args: argparse.Namespace) -> list[Path]:
    basedir = Path(args.basedir).resolve() if args.basedir else default_basedir().resolve()
    exe = basedir / "quakeray"
    if not exe.is_file():
        raise FileNotFoundError(f"Missing Linux runtime binary: {exe}")

    stage_ad_assets(
        basedir,
        ad_dir=args.ad_dir or None,
        saves_dir=args.ad_saves_dir or None,
    )

    save_name = args.save.removesuffix(".sav")
    game_dir = basedir / "ad"
    save_file = game_dir / f"{save_name}.sav"
    if not save_file.is_file():
        raise FileNotFoundError(f"Missing AD save: {save_file}")

    cfg = game_dir / "config.cfg"
    cfg_bak = game_dir / "config.cfg.perf_bak"
    bench = game_dir / "benchmark.log"
    console = basedir / "qconsole.log"
    stderr_file = basedir / "perf_stderr.log"
    modes = ["gpumax", "cpumax"] if args.mode == "both" else [args.mode]
    written_logs: list[Path] = []
    config_backed_up_here = False

    with MachineGuard():
        if cfg_bak.exists():
            raise RuntimeError("RECOVERY_REQUIRED: an existing performance backup needs manual review.")
        try:
            if cfg.is_file():
                shutil.copy2(cfg, cfg_bak)
                config_backed_up_here = True

            with X11Controller() as x11:
                for mode in modes:
                    template = PERF_DIR / f"qr_perf_{mode}.cfg"
                    if not template.is_file():
                        raise FileNotFoundError(f"Mode config not found: {template}")

                    bench_from = len(bench.read_text(encoding="utf-8", errors="replace").splitlines()) if bench.is_file() else 0
                    mode_cfg = game_dir / f"qr_perf_{mode}.cfg"
                    common_cfg = game_dir / "qr_perf_common.cfg"
                    run_cfg = (
                        template.read_text(encoding="utf-8", errors="replace").rstrip("\r\n")
                        + f"\nload {save_name}\necho ==== SAVE_LOADED\n"
                        + f'bind F4 "rt_cluster_sampling 1; load {save_name}; echo ==== RELOAD_S1"\n'
                        + f'bind F5 "rt_cluster_sampling 0; load {save_name}; echo ==== RELOAD_S0"\n'
                        + 'bind F10 "menu_main; toggleconsole; quit; echo ==== QUIT"\n'
                    )
                    mode_cfg.write_text(run_cfg, encoding="ascii")
                    shutil.copy2(PERF_DIR / "qr_perf_common.cfg", common_cfg)

                    for stale in (console, stderr_file):
                        if stale.is_file():
                            stale.unlink()

                    proc: subprocess.Popen[bytes] | None = None
                    try:
                        cmd = [
                            str(exe),
                            "-basedir",
                            str(basedir),
                            "-game",
                            "ad",
                            "-condebug",
                            "+exec",
                            f"qr_perf_{mode}.cfg",
                        ]
                        if args.no_sound:
                            cmd.append("-nosound")

                        with stderr_file.open("wb") as err_f:
                            proc = subprocess.Popen(
                                cmd,
                                cwd=str(basedir),
                                env=display_env(),
                                stdout=subprocess.DEVNULL,
                                stderr=err_f,
                            )
                        print(f"[{mode}] started pid {proc.pid}")

                        waiter = ConsoleWaiter(console)
                        waiter.wait_for_marker(proc, "==== SAVE_LOADED", time.monotonic() + 150.0, stderr_file)
                        print(f"[{mode}] save loaded: True")
                        win_id = x11.wait_for_window(proc, timeout_s=150.0)
                        x11.force_focus(win_id, proc.pid, retries=10, sleep_s=0.25)
                        time.sleep(2 + args.warmup)

                        for arm_key, reload_marker in (("F4", "==== RELOAD_S1"), ("F5", "==== RELOAD_S0")):
                            win_id = _send_key_and_wait_marker(
                                x11,
                                waiter,
                                proc,
                                win_id,
                                arm_key,
                                reload_marker,
                                150.0,
                                stderr_file,
                                ack_marker="Loading game from",
                                retry_after_s=4.0,
                                max_attempts=3,
                            )

                            time.sleep(args.warmup)
                            win_id = _send_key_and_wait_marker(
                                x11,
                                waiter,
                                proc,
                                win_id,
                                "F7",
                                "==== BENCH_START",
                                30.0,
                                stderr_file,
                                retry_after_s=3.0,
                                max_attempts=3,
                            )

                            time.sleep(args.seconds)
                            win_id = _send_key_and_wait_marker(
                                x11,
                                waiter,
                                proc,
                                win_id,
                                "F8",
                                "==== BENCH_STOP",
                                30.0,
                                stderr_file,
                                retry_after_s=3.0,
                                max_attempts=3,
                            )
                            time.sleep(1.0)

                        x11.force_focus(win_id, proc.pid, retries=3, sleep_s=0.15)
                        x11.send_key(win_id, proc.pid, "F10")
                        try:
                            proc.wait(timeout=10.0)
                        except subprocess.TimeoutExpired:
                            kill_process_gracefully(proc)
                        time.sleep(1.0)
                    finally:
                        kill_process_gracefully(proc)
                        for tmp_cfg in (mode_cfg, common_cfg):
                            if tmp_cfg.is_file():
                                tmp_cfg.unlink()

                    if not bench.is_file():
                        raise RuntimeError(f"no benchmark.log found at {bench}")
                    all_lines = bench.read_text(encoding="utf-8", errors="replace").splitlines()
                    new_lines = all_lines[min(bench_from, len(all_lines)) :]
                    if not new_lines:
                        raise RuntimeError(f"no new benchmark blocks written for mode {mode}")

                    label = args.tag if args.tag else save_name
                    safe_name = re.sub(r"[^a-zA-Z0-9_.-]", "_", f"{label}-{mode}")
                    out_log = game_dir / f"perf-{safe_name}.log"
                    out_log.write_text("\n".join(new_lines) + "\n", encoding="utf-8")
                    written_logs.append(out_log)
                    print(f"blocks copied to {out_log}")
                    formatted = format_place_blocks(parse_place_blocks(new_lines))
                    if formatted:
                        print(formatted)
        finally:
            if config_backed_up_here and cfg_bak.is_file():
                shutil.copy2(cfg_bak, cfg)
                cfg_bak.unlink()

    print("perf run finished")
    return written_logs


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    run_place(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
