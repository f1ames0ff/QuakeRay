"""Linux machine guard, X11 focus/input helpers, GPU monitor, and asset staging for QuakeRay perf runners."""

from __future__ import annotations

from contextlib import ContextDecorator
import ctypes
from dataclasses import dataclass
import errno
import fcntl
import glob
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
from typing import Any, Callable


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_LOCK_PATH = Path("/tmp/quakeray-machine-guard.lock")

HEAVY_PROCESS_RE = re.compile(
    r"^(quakeray|qray_.*|cmake|ninja|cc1|cc1plus|g\+\+|gcc|clang|clang\+\+|ld|ld\.lld|mold)$"
)
RUNNER_CMDLINE_RE = re.compile(
    r"(run_stress\.py|run_place\.py|run_menu\.py|run_map_suite\.py|build_linux\.sh)"
)
IGNORED_COMPOSITOR_NAMES = frozenset(
    {
        "Xorg",
        "Xwayland",
        "kwin_x11",
        "kwin_wayland",
        "gnome-shell",
        "mutter",
        "cinnamon",
        "cinnamon-sessio",
        "picom",
        "compiz",
        "xfwm4",
        "plasmashell",
        "sway",
        "weston",
        "wayfire",
    }
)

GENERATED_EXACT_NAMES = frozenset({"config.cfg", "benchmark.log", "qperfdump.log", "crash.log"})
GENERATED_FILE_RE = re.compile(
    r"^(?:stats-\d{8}-\d{6}\.dump|benchmark-frames-[A-Za-z0-9_-]+\.csv|"
    r"screenshot\d+\.png|perf-[A-Za-z0-9_.-]+\.log|(?:qs_|qr_m_)[a-f0-9]+\.cfg)$"
)

AD_PAK_FILES = ("pak0.pak", "pak1.pak", "pak2.pak", "quake.rc")
CANONICAL_AD_SAVES = ("qr_fuma_start.sav", "qr_ad_start.sav", "qr_gpu_heavy.sav", "qr_ad_swamp.sav")
AD_CONFIG_FILES = ("qray.materials.yaml", "qray.lights.yaml")

KEYSYMS: dict[str, int] = {
    "F4": 0xFFC1,
    "F5": 0xFFC2,
    "F6": 0xFFC3,
    "F7": 0xFFC4,
    "F8": 0xFFC5,
    "F9": 0xFFC6,
    "F10": 0xFFC7,
    "F11": 0xFFC8,
    "F12": 0xFFC9,
    "Escape": 0xFF1B,
    "d": 0x0064,
}


def default_basedir() -> Path:
    env_build = os.environ.get("QUAKERAY_BUILD") or os.environ.get("QR_BUILD_DIR")
    if env_build:
        return Path(env_build)
    return REPO_ROOT / "build/linux-debug"


def display_env(extra: dict[str, str] | None = None) -> dict[str, str]:
    env = os.environ.copy()
    if not env.get("DISPLAY"):
        env["DISPLAY"] = ":0"
    env["SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS"] = "0"
    if extra:
        env.update(extra)
    return env


def test_stop_requested() -> bool:
    for var in ("QUAKERAY_JOB_STOP_FILE", "QUAKERAY_STOP_FILE"):
        stop_file = os.environ.get(var)
        if stop_file and Path(stop_file).exists():
            return True
    return False


def test_stress_budget(deadline: float | None, required_seconds: float, now: float | None = None) -> bool:
    if deadline is None:
        return True
    current = time.monotonic() if now is None else now
    return (deadline - current) >= required_seconds


def get_stress_deadline(deadline: float | None, max_seconds: float, now: float | None = None) -> float:
    current = time.monotonic() if now is None else now
    limit = current + max_seconds
    if deadline is not None and deadline < limit:
        return deadline
    return limit


def get_stress_exit_timeout(deadline: float, now: float | None = None, max_ms: int = 15000) -> int:
    current = time.monotonic() if now is None else now
    remaining_ms = int((deadline - current) * 1000.0)
    return max(0, min(max_ms, remaining_ms))


def parse_proc_stat_ppid_and_state(stat_text: str) -> tuple[str, int]:
    rparen = stat_text.rfind(")")
    if rparen < 0:
        raise ValueError("Invalid /proc/<pid>/stat format")
    fields = stat_text[rparen + 1 :].split()
    if len(fields) < 2:
        raise ValueError("Truncated /proc/<pid>/stat fields")
    return fields[0], int(fields[1])


def get_process_ancestor_ids(pid: int | None = None) -> set[int]:
    current = os.getpid() if pid is None else pid
    ancestors: set[int] = set()
    while current > 1 and current not in ancestors:
        ancestors.add(current)
        try:
            stat_text = Path(f"/proc/{current}/stat").read_text(encoding="utf-8", errors="replace")
            _, ppid = parse_proc_stat_ppid_and_state(stat_text)
            current = ppid
        except OSError:
            break
    if current == 1:
        ancestors.add(1)
    return ancestors


def list_processes() -> list[dict[str, Any]]:
    processes: list[dict[str, Any]] = []
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        pid = int(entry)
        proc_dir = Path("/proc") / entry
        try:
            stat_text = (proc_dir / "stat").read_text(encoding="utf-8", errors="replace")
            state, ppid = parse_proc_stat_ppid_and_state(stat_text)
            if state == "Z":
                continue
            comm = (proc_dir / "comm").read_text(encoding="utf-8", errors="replace").strip()
            raw_cmd = (proc_dir / "cmdline").read_bytes()
            args = [part.decode("utf-8", errors="replace") for part in raw_cmd.split(b"\x00") if part]
        except (OSError, ValueError):
            continue
        processes.append(
            {
                "pid": pid,
                "ppid": ppid,
                "comm": comm,
                "args": args,
                "cmdline": " ".join(args),
            }
        )
    return processes


def get_heavy_processes(excluded_pids: set[int] | None = None) -> list[dict[str, Any]]:
    excluded = excluded_pids or set()
    busy: list[dict[str, Any]] = []
    for proc in list_processes():
        if proc["pid"] in excluded:
            continue
        exe_name = Path(proc["args"][0]).name if proc["args"] else ""
        if HEAVY_PROCESS_RE.match(proc["comm"]) or (exe_name and HEAVY_PROCESS_RE.match(exe_name)):
            busy.append(proc)
    return busy


def get_external_runner_processes(excluded_pids: set[int] | None = None) -> list[dict[str, Any]]:
    excluded = excluded_pids or set()
    runners: list[dict[str, Any]] = []
    for proc in list_processes():
        if proc["pid"] in excluded:
            continue
        if proc["cmdline"] and RUNNER_CMDLINE_RE.search(proc["cmdline"]):
            runners.append(proc)
    return runners


def parse_drm_fdinfo(text: str) -> tuple[str | None, int]:
    client_id: str | None = None
    total_ns = 0
    for line in text.splitlines():
        if line.startswith("drm-client-id:"):
            client_id = line.split(":", 1)[1].strip()
        elif line.startswith(("drm-engine-gfx:", "drm-engine-compute:")):
            match = re.search(r":\s*(\d+)\s*ns\b", line)
            if match:
                total_ns += int(match.group(1))
    return client_id, total_ns


def sample_drm_engine_ns() -> dict[tuple[int, str], tuple[str, int]]:
    snapshot: dict[tuple[int, str], tuple[str, int]] = {}
    for fdinfo_path in glob.glob("/proc/*/fdinfo/*"):
        parts = fdinfo_path.split("/")
        if len(parts) < 5 or not parts[2].isdigit():
            continue
        pid = int(parts[2])
        try:
            text = Path(fdinfo_path).read_text(encoding="utf-8", errors="replace")
            if "drm-engine-" not in text:
                continue
            client_id, total_ns = parse_drm_fdinfo(text)
            if not client_id:
                continue
            comm = Path(f"/proc/{pid}/comm").read_text(encoding="utf-8", errors="replace").strip()
        except OSError:
            continue
        key = (pid, client_id)
        prev = snapshot.get(key)
        if prev is None or total_ns > prev[1]:
            snapshot[key] = (comm, total_ns)
    return snapshot


def get_gpu_busy_processes(
    excluded_pids: set[int] | None = None,
    threshold_pct: float = 15.0,
    sample_ms: int = 400,
    ignored_names: frozenset[str] = IGNORED_COMPOSITOR_NAMES,
    sampler: Callable[[], dict[tuple[int, str], tuple[str, int]]] | None = None,
    sleep_fn: Callable[[float], None] = time.sleep,
    clock_fn: Callable[[], float] = time.monotonic,
) -> list[dict[str, Any]]:
    excluded = excluded_pids or set()
    read_sample = sampler or sample_drm_engine_ns
    t0 = clock_fn()
    first = read_sample()
    if sample_ms > 0:
        sleep_fn(sample_ms / 1000.0)
    t1 = clock_fn()
    second = read_sample()
    elapsed_ns = max(1.0, (t1 - t0) * 1e9)

    delta_by_pid: dict[int, int] = {}
    comm_by_pid: dict[int, str] = {}
    for (pid, client_id), (comm, ns1) in second.items():
        if pid in excluded or comm in ignored_names:
            continue
        prev = first.get((pid, client_id))
        if prev is None:
            continue
        delta = max(0, ns1 - prev[1])
        delta_by_pid[pid] = delta_by_pid.get(pid, 0) + delta
        comm_by_pid[pid] = comm

    busy: list[dict[str, Any]] = []
    for pid, delta_ns in sorted(delta_by_pid.items(), key=lambda item: -item[1]):
        pct = (delta_ns / elapsed_ns) * 100.0
        if pct >= threshold_pct:
            busy.append({"pid": pid, "comm": comm_by_pid.get(pid, "?"), "gpu_pct": round(pct, 2)})
    return busy


def parse_xrandr_output(text: str) -> list[dict[str, Any]]:
    displays: list[dict[str, Any]] = []
    current: dict[str, Any] | None = None
    for line in text.splitlines():
        conn = re.match(r"^(\S+)\s+connected\b(.*)$", line)
        if conn:
            if current and current["Width"] > 0 and current["Height"] > 0:
                displays.append(current)
            name = conn.group(1)
            rest = conn.group(2)
            tokens = rest.split()
            primary = "primary" in tokens
            width = 0
            height = 0
            for tok in tokens:
                geom = re.match(r"^(\d+)x(\d+)\+(-?\d+)\+(-?\d+)$", tok)
                if geom:
                    width = int(geom.group(1))
                    height = int(geom.group(2))
                    break
            current = {
                "Name": name,
                "Primary": primary,
                "Width": width,
                "Height": height,
                "RefreshHz": 0,
                "VideoModeDescription": "",
            }
            continue

        if current is None:
            continue
        if not line.startswith((" ", "\t")):
            if current["Width"] > 0 and current["Height"] > 0:
                displays.append(current)
            current = None
            continue

        rate_match = re.search(r"([0-9]+(?:\.[0-9]+)?)\*", line)
        if rate_match:
            res_match = re.match(r"^\s+(\d+)x(\d+)\b", line)
            if res_match and current["Width"] == 0:
                current["Width"] = int(res_match.group(1))
                current["Height"] = int(res_match.group(2))
            hz = int(round(float(rate_match.group(1))))
            current["RefreshHz"] = hz
            current["VideoModeDescription"] = f"{current['Width']} x {current['Height']} @ {hz} Hz"

    if current and current["Width"] > 0 and current["Height"] > 0:
        displays.append(current)
    return displays


def get_active_displays() -> list[dict[str, Any]]:
    proc = subprocess.run(
        ["xrandr", "--query"],
        capture_output=True,
        text=True,
        env=display_env(),
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"MACHINE_GUARD_UNAVAILABLE: xrandr --query failed: {proc.stderr.strip()}")
    displays = parse_xrandr_output(proc.stdout)
    if not displays:
        raise RuntimeError("MACHINE_GUARD_UNAVAILABLE: no active display mode was reported by xrandr.")
    return displays


def normalize_xid(raw: str) -> str | None:
    try:
        value = int(raw, 16)
    except ValueError:
        return None
    if value == 0:
        return None
    return f"0x{value:x}"


def parse_wmctrl_windows(text: str) -> list[dict[str, Any]]:
    windows: list[dict[str, Any]] = []
    for line in text.splitlines():
        parts = line.split(None, 4)
        if len(parts) < 4:
            continue
        win_id = normalize_xid(parts[0])
        if not win_id:
            continue
        try:
            desktop = int(parts[1])
            pid = int(parts[2])
        except ValueError:
            continue
        windows.append(
            {
                "win_id": win_id,
                "raw_win_id": parts[0],
                "desktop": desktop,
                "pid": pid,
                "host": parts[3],
                "title": parts[4] if len(parts) > 4 else "",
            }
        )
    return windows


def parse_xprop_active_window(text: str) -> str | None:
    match = re.search(r"window id #\s*(0x[0-9a-fA-F]+)", text)
    if not match:
        return None
    return normalize_xid(match.group(1))


def parse_xprop_pid(text: str) -> int | None:
    match = re.search(r"_NET_WM_PID\([^)]+\)\s*=\s*(\d+)", text)
    if not match:
        return None
    return int(match.group(1))


class MachineGuard(ContextDecorator):
    """Exclusive flock guard checking heavy builds/runners/GPU load before running."""

    def __init__(
        self,
        timeout_seconds: float = 180.0,
        lock_path: str | Path | None = None,
        check_idle: bool = True,
    ) -> None:
        self.timeout_seconds = timeout_seconds
        self.lock_path = Path(lock_path or os.environ.get("QUAKERAY_GUARD_LOCK") or DEFAULT_LOCK_PATH)
        self.check_idle = check_idle
        self._fd: int | None = None

    def __enter__(self) -> MachineGuard:
        self.lock_path.parent.mkdir(parents=True, exist_ok=True)
        fd = os.open(str(self.lock_path), os.O_CREAT | os.O_RDWR, 0o666)
        deadline = time.monotonic() + max(0.0, self.timeout_seconds)
        logged = False
        try:
            while True:
                try:
                    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise RuntimeError("MACHINE_BUSY: QuakeRay machine guard is occupied; defer this operation.")
                    if not logged:
                        print("Waiting for QuakeRay machine guard lock...")
                        logged = True
                    time.sleep(min(2.0, remaining))

            self._fd = fd
            if self.check_idle:
                ancestors = get_process_ancestor_ids()
                busy = get_heavy_processes(excluded_pids=ancestors)
                if busy:
                    owners = ", ".join(f"{p['pid']}:{p['comm']}" for p in busy)
                    raise RuntimeError(f"MACHINE_BUSY: foreign game/build processes are running: {owners}.")
                runners = get_external_runner_processes(excluded_pids=ancestors)
                if runners:
                    owners = ", ".join(f"{p['pid']}:{p['comm']}" for p in runners)
                    raise RuntimeError(f"MACHINE_BUSY: a non-cooperating runner/build owns the machine: {owners}.")
                gpu_busy = get_gpu_busy_processes(excluded_pids=ancestors)
                if gpu_busy:
                    owners = ", ".join(f"{g['pid']}:{g['comm']}={g['gpu_pct']}%" for g in gpu_busy)
                    raise RuntimeError(f"MACHINE_BUSY: foreign sustained GPU activity is running: {owners}.")
            return self
        except Exception:
            if self._fd is not None:
                try:
                    fcntl.flock(self._fd, fcntl.LOCK_UN)
                except OSError:
                    pass
                self._fd = None
            os.close(fd)
            raise

    def __exit__(self, exc_type: Any, exc: Any, tb: Any) -> None:
        if self._fd is not None:
            try:
                fcntl.flock(self._fd, fcntl.LOCK_UN)
            finally:
                os.close(self._fd)
                self._fd = None


def wait_for_idle(
    deadline: float | None = None,
    excluded_pids: set[int] | None = None,
    gpu_threshold_pct: float = 15.0,
    poll_seconds: float = 5.0,
    timeout_seconds: float = 300.0,
) -> None:
    wait_deadline = get_stress_deadline(deadline, timeout_seconds)
    excluded = get_process_ancestor_ids() if excluded_pids is None else excluded_pids
    reported = False
    while True:
        if test_stop_requested():
            raise RuntimeError("CANCELLED: stop requested while waiting for idle machine.")
        busy = get_heavy_processes(excluded_pids=excluded)
        runners = get_external_runner_processes(excluded_pids=excluded)
        gpu_busy = get_gpu_busy_processes(excluded_pids=excluded, threshold_pct=gpu_threshold_pct)
        if not busy and not runners and not gpu_busy:
            return
        if not reported:
            print(
                f"Waiting for GPU/build owners: heavy={[p['pid'] for p in busy]}; "
                f"runners={[p['pid'] for p in runners]}; gpu={gpu_busy}"
            )
            reported = True
        if time.monotonic() >= wait_deadline:
            raise RuntimeError(
                f"MACHINE_BUSY: foreign activity did not clear before deadline: "
                f"heavy={busy}; runners={runners}; gpu={gpu_busy}"
            )
        time.sleep(min(poll_seconds, max(0.1, wait_deadline - time.monotonic())))


def kill_process_gracefully(proc: subprocess.Popen[Any] | None, timeout_s: float = 5.0) -> None:
    if proc is None or proc.poll() is not None:
        return
    try:
        proc.terminate()
        proc.wait(timeout=timeout_s)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5.0)
    except OSError:
        pass


class _XClientMessageData(ctypes.Union):
    _fields_ = [("b", ctypes.c_char * 20), ("s", ctypes.c_short * 10), ("l", ctypes.c_long * 5)]


class _XClientMessageEvent(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_int),
        ("serial", ctypes.c_ulong),
        ("send_event", ctypes.c_int),
        ("display", ctypes.c_void_p),
        ("window", ctypes.c_ulong),
        ("message_type", ctypes.c_ulong),
        ("format", ctypes.c_int),
        ("data", _XClientMessageData),
    ]


_XERROR_HANDLER = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)


class X11Controller:
    """Queries X11 window focus via wmctrl/xprop and sends XTest key events only when focused."""

    def __init__(self, display_name: str | None = None) -> None:
        self.display_name = display_name or os.environ.get("DISPLAY") or ":0"
        self._env = display_env({"DISPLAY": self.display_name})
        self._x11: Any = None
        self._xtst: Any = None
        self._display: Any = None
        self._noop_err_handler = _XERROR_HANDLER(lambda _d, _e: 0)

    def close(self) -> None:
        if self._x11 is not None and self._display:
            try:
                self._x11.XCloseDisplay(self._display)
            finally:
                self._display = None

    def __enter__(self) -> X11Controller:
        return self

    def __exit__(self, exc_type: Any, exc: Any, tb: Any) -> None:
        self.close()

    def _ensure_x11(self) -> None:
        if self._display:
            return
        x11 = ctypes.CDLL("libX11.so.6")
        xtst = ctypes.CDLL("libXtst.so.6")

        x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x11.XOpenDisplay.restype = ctypes.c_void_p
        x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        x11.XCloseDisplay.restype = ctypes.c_int
        x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
        x11.XDefaultRootWindow.restype = ctypes.c_ulong
        x11.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
        x11.XInternAtom.restype = ctypes.c_ulong
        x11.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_long, ctypes.c_void_p]
        x11.XSendEvent.restype = ctypes.c_int
        x11.XMapRaised.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        x11.XMapRaised.restype = ctypes.c_int
        x11.XRaiseWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        x11.XRaiseWindow.restype = ctypes.c_int
        x11.XSetInputFocus.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
        x11.XSetInputFocus.restype = ctypes.c_int
        x11.XSetErrorHandler.argtypes = [ctypes.c_void_p]
        x11.XSetErrorHandler.restype = ctypes.c_void_p
        x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        x11.XKeysymToKeycode.restype = ctypes.c_uint
        x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
        x11.XSync.restype = ctypes.c_int

        xtst.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
        xtst.XTestFakeKeyEvent.restype = ctypes.c_int

        display = x11.XOpenDisplay(self.display_name.encode("utf-8"))
        if not display:
            raise RuntimeError(f"XOpenDisplay({self.display_name!r}) failed")
        self._x11 = x11
        self._xtst = xtst
        self._display = display

    def _activate_window_xlib(self, win_id: str) -> None:
        win_num = int(win_id, 16)
        self._ensure_x11()
        prev_handler = self._x11.XSetErrorHandler(self._noop_err_handler)
        try:
            root = self._x11.XDefaultRootWindow(self._display)
            atom = self._x11.XInternAtom(self._display, b"_NET_ACTIVE_WINDOW", 0)
            ev = _XClientMessageEvent()
            ev.type = 33  # ClientMessage
            ev.serial = 0
            ev.send_event = 1
            ev.display = self._display
            ev.window = win_num
            ev.message_type = atom
            ev.format = 32
            ev.data.l[0] = 2  # Pager / direct user-like activation (bypasses Cinnamon focus-stealing guard)
            ev.data.l[1] = 0
            ev.data.l[2] = 0
            self._x11.XSendEvent(self._display, root, 0, 0x180000, ctypes.byref(ev))
            self._x11.XMapRaised(self._display, win_num)
            self._x11.XRaiseWindow(self._display, win_num)
            self._x11.XSetInputFocus(self._display, win_num, 1, 0)  # RevertToParent, CurrentTime
            self._x11.XSync(self._display, 0)
        finally:
            self._x11.XSetErrorHandler(prev_handler)

    def find_window_for_pid(self, pid: int) -> str | None:
        proc = subprocess.run(["wmctrl", "-lp"], capture_output=True, text=True, env=self._env, check=False)
        if proc.returncode != 0:
            return None
        matches = [w for w in parse_wmctrl_windows(proc.stdout) if w["pid"] == pid]
        if not matches:
            return None
        return matches[-1]["win_id"]

    def get_active_window_id(self) -> str | None:
        proc = subprocess.run(
            ["xprop", "-root", "_NET_ACTIVE_WINDOW"],
            capture_output=True,
            text=True,
            env=self._env,
            check=False,
        )
        if proc.returncode != 0:
            return None
        return parse_xprop_active_window(proc.stdout)

    def get_window_pid(self, win_id: str) -> int | None:
        proc = subprocess.run(
            ["xprop", "-id", win_id, "_NET_WM_PID"],
            capture_output=True,
            text=True,
            env=self._env,
            check=False,
        )
        if proc.returncode != 0:
            return None
        return parse_xprop_pid(proc.stdout)

    def owns_focus(self, pid: int) -> bool:
        active = self.get_active_window_id()
        if not active:
            return False
        owner_pid = self.get_window_pid(active)
        return owner_pid == pid

    def force_focus(self, win_id: str, pid: int, retries: int = 10, sleep_s: float = 0.3) -> bool:
        for _ in range(max(1, retries)):
            try:
                self._activate_window_xlib(win_id)
            except Exception:
                pass
            subprocess.run(["wmctrl", "-i", "-a", win_id], capture_output=True, text=True, env=self._env, check=False)
            time.sleep(sleep_s)
            if self.owns_focus(pid):
                return True
        return False

    def wait_for_window(self, proc: subprocess.Popen[Any], timeout_s: float = 60.0) -> str:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                raise RuntimeError(f"Runtime exited with code {proc.returncode} before opening a window")
            win_id = self.find_window_for_pid(proc.pid)
            if win_id:
                return win_id
            time.sleep(0.25)
        raise RuntimeError("No runtime window appeared before timeout")

    def set_key_state(self, win_id: str, pid: int, key_name: str, pressed: bool, require_focus: bool = True) -> None:
        if key_name not in KEYSYMS:
            raise ValueError(f"Unknown key name: {key_name}")
        if require_focus and not self.owns_focus(pid):
            if not self.force_focus(win_id, pid, retries=5, sleep_s=0.15):
                raise RuntimeError(f"Refusing to inject XTest key {key_name!r}: runtime pid {pid} lacks focus")
        self._ensure_x11()
        keycode = self._x11.XKeysymToKeycode(self._display, KEYSYMS[key_name])
        if not keycode:
            raise RuntimeError(f"XKeysymToKeycode failed for {key_name!r}")
        self._xtst.XTestFakeKeyEvent(self._display, keycode, 1 if pressed else 0, 0)
        self._x11.XSync(self._display, 0)

    def send_key(self, win_id: str, pid: int, key_name: str, hold_ms: int = 60, after_ms: int = 250) -> None:
        self.set_key_state(win_id, pid, key_name, True, require_focus=True)
        time.sleep(hold_ms / 1000.0)
        # Always release without blocking on a transient focus shift so no key stays stuck down.
        self.set_key_state(win_id, pid, key_name, False, require_focus=False)
        if after_ms > 0:
            time.sleep(after_ms / 1000.0)


def sha256_file(path: Path, upper: bool = True) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    hex_str = digest.hexdigest()
    return hex_str.upper() if upper else hex_str


def _find_case_insensitive_file(directory: Path, filename: str) -> Path | None:
    direct = directory / filename
    if direct.is_file():
        return direct
    target_lower = filename.lower()
    try:
        for item in directory.iterdir():
            if item.name.lower() == target_lower and item.is_file():
                return item
    except OSError:
        return None
    return None


def stage_file_hardlink_or_copy(src: Path, dst: Path) -> str:
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.is_symlink():
        dst.unlink()
    elif dst.exists():
        src_stat = src.stat()
        dst_stat = dst.stat()
        if (src_stat.st_dev, src_stat.st_ino) == (dst_stat.st_dev, dst_stat.st_ino):
            return "skipped"
        if src_stat.st_size == dst_stat.st_size and sha256_file(src) == sha256_file(dst):
            return "skipped"
        dst.unlink()
    try:
        os.link(src, dst)
        return "hardlinked"
    except OSError as exc:
        if exc.errno not in (errno.EXDEV, errno.EPERM, errno.EACCES):
            raise
        shutil.copy2(src, dst)
        return "copied"


def _resolve_existing_dir(candidates: list[Path | str | None]) -> Path | None:
    for item in candidates:
        if not item:
            continue
        p = Path(item)
        if p.is_dir():
            return p
    return None


def stage_ad_assets(
    basedir: str | Path,
    ad_dir: str | Path | None = None,
    saves_dir: str | Path | None = None,
    materials_dir: str | Path | None = None,
    overwrite_existing: bool = False,
) -> dict[str, str]:
    base = Path(basedir).resolve()
    ad_target = base / "ad"
    ad_target.mkdir(parents=True, exist_ok=True)

    resolved_ad_dir = _resolve_existing_dir(
        [ad_dir, os.environ.get("QR_AD_DIR"), Path.home() / "Games/quake-ad", REPO_ROOT / "build/Release/ad"]
    )
    resolved_saves_dir = _resolve_existing_dir(
        [saves_dir, os.environ.get("QR_AD_SAVES_DIR"), REPO_ROOT / "build/Release/ad", REPO_ROOT / "build/Debug/ad"]
    )
    resolved_materials_dir = _resolve_existing_dir(
        [materials_dir, os.environ.get("QR_AD_MATERIALS_DIR"), REPO_ROOT / "build/Debug/ad", REPO_ROOT / "build/Release/ad"]
    )

    actions: dict[str, str] = {}
    groups: list[tuple[Path | None, tuple[str, ...], bool]] = [
        (resolved_ad_dir, AD_PAK_FILES, overwrite_existing or ad_dir is not None),
        (resolved_saves_dir, CANONICAL_AD_SAVES, overwrite_existing or saves_dir is not None),
        (resolved_materials_dir, AD_CONFIG_FILES, overwrite_existing or materials_dir is not None),
    ]
    for src_dir, names, allow_replace in groups:
        if src_dir is None:
            continue
        for name in names:
            dst = ad_target / name
            if dst.exists() and not dst.is_symlink() and not allow_replace:
                actions[name] = "kept"
                continue
            src = _find_case_insensitive_file(src_dir, name)
            if src is None:
                continue
            if dst.is_symlink() and dst.resolve().is_relative_to(base) and not allow_replace:
                actions[name] = "kept"
                continue
            actions[name] = stage_file_hardlink_or_copy(src, dst)
    return actions


def build_asset_inventory(runtime: str | Path) -> list[dict[str, str]]:
    """Mirror tools/quakeray_mcp/src/quakeray_mcp/asset_inventory.py without MCP imports."""
    runtime_path = Path(runtime).resolve()
    entries: list[dict[str, str]] = []
    for directory in ("id1", "ad"):
        root_dir = runtime_path / directory
        if not root_dir.is_dir():
            continue
        for path in sorted(root_dir.rglob("*")):
            if not path.is_file():
                continue
            name = path.name.lower()
            rel = path.relative_to(runtime_path)
            direct_game_file = len(rel.parts) == 2
            generated = name in GENERATED_EXACT_NAMES or GENERATED_FILE_RE.fullmatch(name) is not None
            if direct_game_file and generated:
                continue
            resolved = path.resolve()
            if not resolved.is_relative_to(runtime_path):
                raise PermissionError(f"Asset inventory contains a redirected path: {rel.as_posix()} -> {resolved}")
            entries.append({"File": rel.as_posix(), "Sha256": sha256_file(path, upper=True)})
            if len(entries) > 10000:
                raise RuntimeError("Asset inventory exceeds 10000 entry bound")
    return entries


@dataclass
class ConsoleWaiter:
    console_path: Path

    def count_marker(self, marker: str) -> int:
        if not self.console_path.is_file():
            return 0
        text = self.console_path.read_text(encoding="utf-8", errors="replace")
        return text.count(marker)

    def wait_for_marker(
        self,
        proc: subprocess.Popen[Any],
        marker: str,
        deadline: float,
        stderr_path: Path | None = None,
        min_count: int = 1,
    ) -> None:
        while time.monotonic() < deadline:
            if test_stop_requested():
                raise RuntimeError(f"CANCELLED: stop requested while awaiting marker {marker!r}.")
            if proc.poll() is not None:
                raise RuntimeError(f"Runtime exited with code {proc.returncode} before {marker}")
            if stderr_path and stderr_path.is_file():
                failure = stderr_path.read_text(encoding="utf-8", errors="replace")
                if re.search(r"Vulkan call failed|FATAL|Sys_Error", failure):
                    raise RuntimeError(f"Runtime startup error: {failure.strip()}")
            if self.count_marker(marker) >= min_count:
                return
            time.sleep(0.25)
        raise RuntimeError(f"Timed out waiting for {marker}")
