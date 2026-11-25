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

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time


parser = argparse.ArgumentParser()
parser.add_argument("mode", choices=["sleep", "fail", "child", "marker", "cooperate"])
parser.add_argument("--file", type=Path)
parser.add_argument("--seconds", type=float, default=30)
parser.add_argument("--stop-file", type=Path)
args = parser.parse_args()
if args.mode == "cooperate":
    while not args.stop_file.is_file():
        time.sleep(0.02)
    raise SystemExit(0)
if args.mode == "fail":
    raise SystemExit(7)
if args.mode == "marker":
    args.file.write_text("executed", encoding="utf-8")
    raise SystemExit(0)
if args.mode == "child":
    child = subprocess.Popen([sys.executable, "-B", __file__, "sleep", "--seconds", "30"])
    args.file.write_text(json.dumps({"parent": os.getpid(), "child": child.pid}), encoding="utf-8")
elif args.file:
    args.file.write_text(str(os.getpid()), encoding="utf-8")
time.sleep(args.seconds)
