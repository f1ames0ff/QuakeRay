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

import os
from pathlib import Path
import signal
import subprocess
import sys

from .errors import EvidenceError


_POSIX_SUPERVISOR = """
import os, select, signal, subprocess, sys
gate_r, ctrl_r, ready_w = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
cwd, stdout_p, stderr_p = sys.argv[4], sys.argv[5], sys.argv[6]
cmd = sys.argv[7:]
try:
    token = os.read(gate_r, 1)
finally:
    os.close(gate_r)
if token != b"G":
    sys.exit(240)
with open(os.devnull, "rb") as devnull, open(stdout_p, "wb") as out_f, open(stderr_p, "wb") as err_f:
    proc = subprocess.Popen(cmd, cwd=cwd, stdin=devnull, stdout=out_f, stderr=err_f, start_new_session=True)
os.write(ready_w, f"{proc.pid}\\n".encode("ascii"))
os.close(ready_w)
while True:
    rc = proc.poll()
    if rc is not None:
        sys.exit(rc)
    rlist, _, _ = select.select([ctrl_r], [], [], 0.05)
    if rlist:
        try:
            data = os.read(ctrl_r, 1)
        except OSError:
            data = b""
        if not data:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except OSError:
                pass
            try:
                proc.wait(timeout=2)
            except Exception:
                pass
            sys.exit(240)
""".strip()


def _posix_starttime(pid: int) -> str | None:
    try:
        text = Path(f"/proc/{pid}/stat").read_text(encoding="utf-8", errors="replace")
        rparen = text.rfind(")")
        if rparen < 0:
            return None
        fields = text[rparen + 1 :].split()
        if len(fields) < 20 or fields[0] == "Z":
            return None
        return fields[19]
    except (OSError, ValueError):
        return None


class ContainedProcess:
    def __init__(self, command, cwd, stdout, stderr, assign=None):
        if os.name == "nt":
            self._init_windows(command, cwd, stdout, stderr, assign=assign)
        else:
            self._init_posix(command, cwd, stdout, stderr, assign=assign)

    def _init_windows(self, command, cwd, stdout, stderr, assign=None):
        import pywintypes
        import win32api
        import win32con
        import win32file
        import win32event
        import win32job
        import win32process

        self._posix = False
        self.job = None
        self.process = None
        self.pid = None
        handles = []
        thread = None
        try:
            self.job = win32job.CreateJobObject(None, "")
            win32api.SetHandleInformation(self.job, win32con.HANDLE_FLAG_INHERIT, 0)
            limits = win32job.QueryInformationJobObject(self.job, win32job.JobObjectExtendedLimitInformation)
            limits["BasicLimitInformation"]["LimitFlags"] |= win32job.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            win32job.SetInformationJobObject(self.job, win32job.JobObjectExtendedLimitInformation, limits)
            security = pywintypes.SECURITY_ATTRIBUTES()
            security.bInheritHandle = True
            for path, access in (("NUL", win32con.GENERIC_READ), (str(stdout), win32con.GENERIC_WRITE),
                                 (str(stderr), win32con.GENERIC_WRITE)):
                handles.append(win32file.CreateFile(path, access, win32con.FILE_SHARE_READ | win32con.FILE_SHARE_WRITE,
                                                   security, win32con.OPEN_EXISTING if path == "NUL" else win32con.CREATE_ALWAYS,
                                                   win32con.FILE_ATTRIBUTE_NORMAL, None))
            startup = win32process.STARTUPINFO()
            startup.dwFlags |= win32con.STARTF_USESTDHANDLES
            startup.hStdInput, startup.hStdOutput, startup.hStdError = handles
            self.process, thread, self.pid, _ = win32process.CreateProcess(
                None, subprocess.list2cmdline(command), None, None, True,
                win32con.CREATE_SUSPENDED | win32con.CREATE_NO_WINDOW, None, str(cwd), startup)
            (assign or win32job.AssignProcessToJobObject)(self.job, self.process)
            self.creation_time = str(win32process.GetProcessTimes(self.process)["CreationTime"])
            win32process.ResumeThread(thread)
        except Exception as exc:
            if self.process:
                win32process.TerminateProcess(self.process, 240)
                win32event.WaitForSingleObject(self.process, 5000)
            self.close()
            raise EvidenceError("PROCESS_CONTAINMENT_FAILED", "Cannot safely contain the worker process") from exc
        finally:
            if thread:
                thread.Close()
            for handle in handles:
                handle.Close()

    def _init_posix(self, command, cwd, stdout, stderr, assign=None):
        self._posix = True
        self.supervisor = None
        self.pid = None
        self.creation_time = None
        self._ctrl_w = None

        gate_r, gate_w = os.pipe()
        ctrl_r, ctrl_w = os.pipe()
        ready_r, ready_w = os.pipe()
        try:
            self.supervisor = subprocess.Popen(
                [sys.executable, "-B", "-c", _POSIX_SUPERVISOR,
                 str(gate_r), str(ctrl_r), str(ready_w),
                 str(cwd), str(stdout), str(stderr), *[str(part) for part in command]],
                pass_fds=(gate_r, ctrl_r, ready_w),
                start_new_session=True,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            os.close(gate_r); gate_r = -1
            os.close(ctrl_r); ctrl_r = -1
            os.close(ready_w); ready_w = -1
            self._ctrl_w = ctrl_w

            if assign is not None:
                assign(self.supervisor.pid, self.supervisor)

            os.write(gate_w, b"G")
            os.close(gate_w); gate_w = -1

            chunks = []
            while True:
                buf = os.read(ready_r, 64)
                if not buf:
                    break
                chunks.append(buf)
                if b"\n" in buf:
                    break
            os.close(ready_r); ready_r = -1
            line = b"".join(chunks).decode("ascii", errors="replace").strip()
            if not line:
                raise RuntimeError("Supervisor exited before starting worker")
            self.pid = int(line.splitlines()[0])
            self.creation_time = _posix_starttime(self.pid)
        except Exception as exc:
            for fd in (gate_r, gate_w, ctrl_r, ready_r, ready_w):
                if fd >= 0:
                    try:
                        os.close(fd)
                    except OSError:
                        pass
            self.close()
            raise EvidenceError("PROCESS_CONTAINMENT_FAILED", "Cannot safely contain the worker process") from exc

    def wait(self, milliseconds=0):
        if not self._posix:
            import win32event
            import win32process
            if win32event.WaitForSingleObject(self.process, milliseconds) == win32event.WAIT_TIMEOUT:
                return None
            return win32process.GetExitCodeProcess(self.process)
        if self.supervisor is None:
            return 240
        if milliseconds <= 0:
            return self.supervisor.poll()
        try:
            return self.supervisor.wait(timeout=milliseconds / 1000.0)
        except subprocess.TimeoutExpired:
            return None

    def terminate(self):
        if not self._posix:
            import win32job
            if self.job:
                win32job.TerminateJobObject(self.job, 240)
            return
        if self.pid:
            try:
                os.killpg(self.pid, signal.SIGKILL)
            except OSError:
                pass
        if self._ctrl_w is not None:
            try:
                os.close(self._ctrl_w)
            except OSError:
                pass
            self._ctrl_w = None

    def close(self):
        if not self._posix:
            if getattr(self, "job", None):
                self.job.Close()
                self.job = None
            if getattr(self, "process", None):
                self.process.Close()
                self.process = None
            return
        if self._ctrl_w is not None:
            try:
                os.close(self._ctrl_w)
            except OSError:
                pass
            self._ctrl_w = None
        if getattr(self, "supervisor", None) is not None:
            try:
                self.supervisor.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.supervisor.kill()
                self.supervisor.wait(timeout=2)
            self.supervisor = None
