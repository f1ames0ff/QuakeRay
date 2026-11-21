#!/usr/bin/env python3
"""Unit tests for diff-gated quality checks in QuakeRay."""

from __future__ import annotations

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_quality as cq  # noqa: E402


class CheckQualityTests(unittest.TestCase):
    def test_parse_git_diff_hunks(self) -> None:
        diff = "\n".join([
            "diff --git a/Quake/rt_light.c b/Quake/rt_light.c",
            "--- a/Quake/rt_light.c",
            "+++ b/Quake/rt_light.c",
            "@@ -10,0 +11,3 @@",
            "@@ -42,2 +45 @@",
            "diff --git a/Quake/old.c b/dev/null",
            "--- a/Quake/old.c",
            "+++ /dev/null",
            "@@ -1,5 +0,0 @@",
        ])
        parsed = cq.parse_git_diff_hunks(diff)
        self.assertEqual(parsed, {"Quake/rt_light.c": {11, 12, 13, 45}})

    def test_exclusions_skip_vendored_and_submodule(self) -> None:
        self.assertTrue(cq.is_excluded("third_party/imgui/imgui.cpp"))
        self.assertTrue(cq.is_excluded("renderer/Source/VulkanDevice.cpp"))
        self.assertTrue(cq.is_excluded("Quake/mimalloc/alloc.c"))
        self.assertFalse(cq.is_excluded("Quake/qr_editor.c"))
        self.assertFalse(cq.is_excluded("tests/rt_lighting_tests.c"))

    def test_clang_tidy_filters_to_changed_lines(self) -> None:
        root = Path("/repo")
        out = "\n".join([
            "/repo/Quake/rt_light.c:25:5: warning: uninitialized value [clang-analyzer-core.uninitialized.Assign]",
            "/repo/Quake/rt_light.c:99:5: warning: legacy issue untouched",
            "/repo/Quake/other.c:10:1: error: untouched file",
        ])
        findings = cq.parse_clang_tidy_output(out, root, {"Quake/rt_light.c": {24, 25, 26}})
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].line, 25)
        self.assertEqual(findings[0].rule, "clang-analyzer-core.uninitialized.Assign")

    def test_cppcheck_filters_warnings_and_keeps_changed_file_errors(self) -> None:
        root = Path("/repo")
        out = "\n".join([
            "Quake/rt_light.c:12:warning:unreadVariable:Variable 'x' is assigned a value that is never used.",
            "Quake/rt_light.c:80:warning:unusedStructMember:Legacy warning outside diff.",
            "Quake/rt_light.c:40:error:nullPointer:Null pointer dereference.",
            "Quake/other.c:5:error:memleak:Memory leak.",
        ])
        findings = cq.parse_cppcheck_output(out, root, {"Quake/rt_light.c": {12}})
        self.assertEqual([(f.line, f.rule) for f in findings], [(12, "unreadVariable"), (40, "nullPointer")])

    def test_lizard_complexity_ratchet(self) -> None:
        limits = cq.QualityLimits(ccn_max=15, nloc_max=120, params_max=7)
        cur = [
            cq.FuncMetric("clean_new", 2, 20, 4, 1, 20),
            cq.FuncMetric("bloated_new", 8, 130, 18, 25, 160),
            cq.FuncMetric("legacy_unchanged", 4, 150, 20, 200, 355),
            cq.FuncMetric("legacy_worsened", 4, 150, 22, 400, 555),
        ]
        base = [
            cq.FuncMetric("legacy_unchanged", 4, 148, 20, 200, 353),
            cq.FuncMetric("legacy_worsened", 4, 140, 20, 400, 545),
        ]
        changed = {10, 30, 210, 410}
        findings = cq.evaluate_complexity_ratchet("Quake/rt_light.c", cur, base, changed, limits)
        rules = {(f.line, f.rule) for f in findings}
        self.assertEqual(
            rules,
            {
                (25, "ccn"),
                (25, "nloc"),
                (25, "params"),
                (400, "ccn-regression"),
                (400, "nloc-regression"),
            },
        )

    def test_ruff_filters_lines_and_catches_undefined_name(self) -> None:
        root = Path("/repo")
        out = "\n".join([
            "/repo/tools/helper.py:4:1: F401 `sys` imported but unused",
            "/repo/tools/helper.py:99:1: F401 legacy unused import",
            "/repo/tools/helper.py:88:5: F821 Undefined name `boom`",
        ])
        findings = cq.parse_ruff_output(out, root, {"tools/helper.py": {4}})
        self.assertEqual([(f.line, f.rule) for f in findings], [(4, "F401"), (88, "F821")])

    def test_windows_drive_prefix_and_ref_validation(self) -> None:
        self.assertEqual(
            cq._split_diag_line("C:\\repo\\Quake\\rt_light.c:12:3: warning: msg [r]", 5),
            ["C:\\repo\\Quake\\rt_light.c", "12", "3", " warning", " msg [r]"],
        )
        repo_root = Path(__file__).resolve().parents[1]
        self.assertEqual(cq.resolve_base_ref(repo_root, "HEAD"), "HEAD")
        with self.assertRaises(ValueError):
            cq.resolve_base_ref(repo_root, "--upload-pack=evil")
        with self.assertRaises(ValueError):
            cq.resolve_base_ref(repo_root, "HEAD..master")


if __name__ == "__main__":
    unittest.main()
