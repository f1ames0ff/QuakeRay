import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time


parser = argparse.ArgumentParser()
parser.add_argument("mode", choices=["sleep", "fail", "child", "marker"])
parser.add_argument("--file", type=Path)
parser.add_argument("--seconds", type=float, default=30)
args = parser.parse_args()
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
