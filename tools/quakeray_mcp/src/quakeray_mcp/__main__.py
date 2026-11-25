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
import logging
import os
from pathlib import Path
import sys

import anyio
from mcp.server.stdio import stdio_server

from .schemas import INPUTS
from .server import create_server
from .service import EvidenceService


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(os.environ.get("QUAKERAY_ROOT", ".")))
    parser.add_argument("--import-root", type=Path, action="append", default=[])
    parser.add_argument("--selfcheck", action="store_true")
    parser.add_argument("--inspect", type=Path)
    arguments = parser.parse_args()
    logging.basicConfig(stream=sys.stderr, level=logging.WARNING)
    roots = arguments.import_root + [Path(p) for p in os.environ.get("QUAKERAY_IMPORT_ROOTS", "").split(os.pathsep) if p]
    try:
        service = EvidenceService(arguments.root, roots)
        if arguments.selfcheck:
            print(json.dumps({"phase": service.server_status()['phase'], "tools": service.tool_names,
                              "documents": service.get_architecture(), "runtime_launch": bool(service.jobs)}, allow_nan=False))
            return
        if arguments.inspect:
            print(json.dumps(service.read_stats({"import_path": str(arguments.inspect)}), allow_nan=False))
            return
        server = create_server(service)

        async def serve():
            async with stdio_server() as (read_stream, write_stream):
                await server.run(read_stream, write_stream, server.create_initialization_options())

        try:
            anyio.run(serve)
        finally:
            service.close()
    except Exception as exc:
        logging.error("Cannot start QuakeRay MCP: %s", exc)
        raise SystemExit(1) from exc


if __name__ == "__main__":
    main()
