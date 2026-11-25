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

"""Unit tests for QuakeRay copyright header checking and auto-insertion."""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import copyright_header as ch  # noqa: E402


class CopyrightHeaderTests(unittest.TestCase):
    def test_insert_header_slash_and_hash_and_empty(self) -> None:
        c_out = ch.insert_header(b"#pragma once\n", ".h", year=2026)
        assert c_out is not None
        c_text = c_out.decode("utf-8")
        self.assertTrue(c_text.startswith("// Copyright (c) 2026 f1ames0ff "))
        self.assertIn("//\n\n#pragma once\n", c_text)

        sh_out = ch.insert_header(b"echo ok\n", ".sh", year=2026)
        assert sh_out is not None
        sh_text = sh_out.decode("utf-8")
        self.assertTrue(sh_text.startswith("# Copyright (c) 2026 f1ames0ff "))
        self.assertIn("#\n\necho ok\n", sh_text)

        empty_out = ch.insert_header(b"", ".c", year=2026)
        assert empty_out is not None
        self.assertTrue(empty_out.decode("utf-8").endswith("//\n"))
        self.assertFalse(empty_out.decode("utf-8").endswith("\n\n"))

    def test_preserves_shebang_coding_bom_crlf_and_ps_requires(self) -> None:
        py_src = b"#!/usr/bin/env python3\n# -*- coding: utf-8 -*-\nprint(1)\n"
        py_out = ch.insert_header(py_src, ".py", year=2026)
        assert py_out is not None
        lines = py_out.decode("utf-8").splitlines()
        self.assertEqual(lines[0], "#!/usr/bin/env python3")
        self.assertEqual(lines[1], "# -*- coding: utf-8 -*-")
        self.assertTrue(lines[2].startswith("# Copyright (c) 2026 "))
        self.assertEqual(lines[-1], "print(1)")

        ps_src = b"#Requires -Version 7.0\r\nWrite-Host 'hi'\r\n"
        ps_out = ch.insert_header(ps_src, ".ps1", year=2026)
        assert ps_out is not None
        ps_lines = ps_out.decode("utf-8").splitlines()
        self.assertEqual(ps_lines[0], "#Requires -Version 7.0")
        self.assertTrue(ps_lines[1].startswith("# Copyright (c) 2026 "))
        self.assertEqual(ps_lines[-1], "Write-Host 'hi'")

    def test_exclusions_cover_third_party_submodule_and_legacy_upstream(self) -> None:
        self.assertFalse(ch.is_eligible_path("third_party/imgui/imgui.cpp"))
        self.assertFalse(ch.is_eligible_path("renderer/Source/VulkanDevice.cpp"))
        self.assertFalse(ch.is_eligible_path("Quake/miniz.h"))
        self.assertFalse(ch.is_eligible_path("Quake/snd_mp3.h"))
        self.assertTrue(ch.is_eligible_path("perf/new_bench.ps1"))
        self.assertTrue(ch.is_eligible_path("Quake/qr_editor.c"))

    def test_staged_mode_adds_and_restages_clean_files_and_guards_partial(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            subprocess.run(["git", "-C", tmp, "init", "-q"], check=True)
            subprocess.run(["git", "-C", tmp, "config", "user.name", "Test"], check=True)
            subprocess.run(["git", "-C", tmp, "config", "user.email", "test@example.com"], check=True)
            (repo / "README.md").write_text("init\n", encoding="utf-8")
            subprocess.run(["git", "-C", tmp, "add", "README.md"], check=True)
            subprocess.run(["git", "-C", tmp, "commit", "-q", "-m", "init"], check=True)

            clean = repo / "Quake/new_mod.c"
            clean.parent.mkdir(parents=True)
            clean.write_text("int f(void) { return 1; }\n", encoding="utf-8")
            empty = repo / "Quake/empty.h"
            empty.write_text("", encoding="utf-8")
            subprocess.run(["git", "-C", tmp, "add", "Quake/new_mod.c", "Quake/empty.h"], check=True)

            self.assertEqual(ch.handle_staged(repo, year=2026), 0)
            self.assertIn("Copyright (c) 2026 f1ames0ff", clean.read_text(encoding="utf-8"))
            staged_blob = subprocess.check_output(
                ["git", "-C", tmp, "cat-file", "blob", ":Quake/new_mod.c"], text=True
            )
            self.assertIn("Copyright (c) 2026 f1ames0ff", staged_blob)
            self.assertIn("Copyright (c) 2026 f1ames0ff", empty.read_text(encoding="utf-8"))

            partial = repo / "Quake/partial.c"
            partial.write_text("int a = 1;\n", encoding="utf-8")
            subprocess.run(["git", "-C", tmp, "add", "Quake/partial.c"], check=True)
            partial.write_text("int a = 2;\n", encoding="utf-8")
            self.assertEqual(ch.handle_staged(repo, year=2026), 1)
            self.assertEqual(partial.read_text(encoding="utf-8"), "int a = 2;\n")


if __name__ == "__main__":
    unittest.main()
