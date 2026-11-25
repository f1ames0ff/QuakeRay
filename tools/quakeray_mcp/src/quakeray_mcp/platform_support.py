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

"""Platform abstractions shared across Windows and POSIX/Linux in QuakeRay MCP."""

from __future__ import annotations

import os
from pathlib import Path
import re
import stat


IS_WINDOWS = os.name == "nt"
EXE_NAME = "quakeray.exe" if IS_WINDOWS else "quakeray"
DEFAULT_BUILD_SUBDIR = "build/Debug" if IS_WINDOWS else "build/linux-debug"
HEAVY_PROCESS_RE = re.compile(
    r"^(quakeray|qray_.*|cmake|ninja|cc1|cc1plus|g\+\+|gcc|clang|clang\+\+|ld|ld\.lld|mold)$"
)


def default_state_root(repo_id: str) -> Path:
    if IS_WINDOWS:
        return Path(os.environ["LOCALAPPDATA"]) / "QuakeRayMCP" / repo_id / "jobs"
    xdg_state = os.environ.get("XDG_STATE_HOME")
    base = Path(xdg_state) if xdg_state else (Path.home() / ".local/state")
    return base / "quakeray_mcp" / repo_id / "jobs"


def is_redirected_path(path: Path) -> bool:
    if path.is_symlink():
        return True
    reparse_flag = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)
    if reparse_flag:
        try:
            return bool(getattr(path.lstat(), "st_file_attributes", 0) & reparse_flag)
        except OSError:
            return False
    return False
