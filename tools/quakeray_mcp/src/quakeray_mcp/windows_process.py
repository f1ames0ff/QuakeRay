import os
import subprocess

from .errors import EvidenceError


class ContainedProcess:
    def __init__(self, command, cwd, stdout, stderr, assign=None):
        if os.name != "nt":
            raise EvidenceError("PROCESS_CONTAINMENT_FAILED", "Windows Job Objects are required")
        import pywintypes
        import win32api
        import win32con
        import win32file
        import win32event
        import win32job
        import win32process

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

    def wait(self, milliseconds=0):
        import win32event
        import win32process
        if win32event.WaitForSingleObject(self.process, milliseconds) == win32event.WAIT_TIMEOUT:
            return None
        return win32process.GetExitCodeProcess(self.process)

    def terminate(self):
        import win32job
        if self.job:
            win32job.TerminateJobObject(self.job, 240)

    def close(self):
        if self.job:
            self.job.Close()
            self.job = None
        if self.process:
            self.process.Close()
            self.process = None
