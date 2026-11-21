#!/usr/bin/env python3
"""Diff-gated code quality and complexity checks for QuakeRay."""

from __future__ import annotations

import argparse
from collections import defaultdict
import csv
from dataclasses import dataclass
import io
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

DEFAULT_BASE = "origin/master"
EXCLUDE_PREFIXES = (
    "third_party/",
    "renderer/",
    "Windows/",
    "Quake/mimalloc/",
    "tools/quakeray_mcp/.venv/",
    "build/",
    "build-",
    "perf/",
)

C_CPP_SOURCE_EXTS = {".c", ".cc", ".cpp", ".cxx"}
C_CPP_HEADER_EXTS = {".h", ".hh", ".hpp", ".hxx"}
C_CPP_EXTS = C_CPP_SOURCE_EXTS | C_CPP_HEADER_EXTS
PY_EXTS = {".py"}

HUNK_RE = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")
CLANG_TIDY_RE = re.compile(r"^((?:[A-Za-z]:)?[^:\r\n]+):(\d+):(\d+):[ \t]+(warning|error):[ \t]+([^\r\n]+)$")
CPPCHECK_RE = re.compile(r"^((?:[A-Za-z]:)?[^:\r\n]+):(\d+):([^:\r\n]+):([^:\r\n]+):([^\r\n]*)$")
RUFF_RE = re.compile(r"^((?:[A-Za-z]:)?[^:\r\n]+):(\d+):\d+:[ \t]+([A-Z]\d+)[ \t]+([^\r\n]+)$")


@dataclass(frozen=True)
class Finding:
    tool: str
    path: str
    line: int
    severity: str
    rule: str
    message: str


@dataclass(frozen=True)
class FuncMetric:
    name: str
    params: int
    nloc: int
    ccn: int
    start_line: int
    end_line: int


@dataclass(frozen=True)
class QualityLimits:
    ccn_max: int
    nloc_max: int
    params_max: int


def norm_rel(path_str: str, repo_root: Path) -> str:
    """Normalize a path into a POSIX path relative to repo_root when inside it."""
    raw = path_str.replace("\\", "/")
    candidate = Path(raw)
    if candidate.is_absolute():
        try:
            return candidate.resolve().relative_to(repo_root.resolve()).as_posix()
        except ValueError:
            return candidate.as_posix()
    if raw.startswith("./"):
        raw = raw[2:]
    resolved = (repo_root / raw).resolve()
    try:
        return resolved.relative_to(repo_root.resolve()).as_posix()
    except ValueError:
        return raw


def is_excluded(rel_path: str, exclude_prefixes: tuple[str, ...] = EXCLUDE_PREFIXES) -> bool:
    norm = rel_path.replace("\\", "/").lstrip("./")
    return any(norm == prefix.rstrip("/") or norm.startswith(prefix) for prefix in exclude_prefixes)


def parse_git_diff_hunks(diff_text: str) -> dict[str, set[int]]:
    """Parse `git diff -U0` output into {rel_path: {1-based changed line numbers}}."""
    changed: dict[str, set[int]] = defaultdict(set)
    current_path: str | None = None
    for line in diff_text.splitlines():
        if line.startswith("+++ "):
            target = line[4:].strip()
            if target == "/dev/null":
                current_path = None
            elif target.startswith("b/"):
                current_path = target[2:].replace("\\", "/")
            else:
                current_path = target.replace("\\", "/")
            continue
        if current_path is None:
            continue
        match = HUNK_RE.match(line)
        if not match:
            continue
        start = int(match.group(1))
        count = int(match.group(2)) if match.group(2) is not None else 1
        if count > 0:
            changed[current_path].update(range(start, start + count))
    return dict(changed)


def _run_git(repo_root: Path, args: list[str], *, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", "-C", str(repo_root), *args],
        capture_output=True,
        text=True,
        errors="replace",
        check=check,
    )


def collect_changed_lines(
    repo_root: Path,
    base_ref: str,
    local: bool,
    exclude_prefixes: tuple[str, ...] = EXCLUDE_PREFIXES,
) -> dict[str, set[int]]:
    """Return changed lines in non-excluded tracked/added files."""
    if local:
        diff_res = _run_git(repo_root, ["diff", "-U0", "HEAD"], check=True)
        changed = parse_git_diff_hunks(diff_res.stdout)
        untracked = _run_git(repo_root, ["ls-files", "--others", "--exclude-standard"], check=True)
        for rel in untracked.stdout.splitlines():
            rel = rel.strip().replace("\\", "/")
            if not rel or is_excluded(rel, exclude_prefixes):
                continue
            full = repo_root / rel
            if full.is_file():
                line_count = max(1, len(full.read_bytes().splitlines()))
                changed.setdefault(rel, set()).update(range(1, line_count + 1))
    else:
        diff_res = _run_git(repo_root, ["diff", "-U0", f"{base_ref}...HEAD"], check=True)
        changed = parse_git_diff_hunks(diff_res.stdout)

    filtered: dict[str, set[int]] = {}
    for rel, lines in changed.items():
        if not is_excluded(rel, exclude_prefixes) and (repo_root / rel).is_file() and lines:
            filtered[rel] = lines
    return filtered


def resolve_tool_cmd(tool: str) -> list[str] | None:
    """Resolve an executable directly from PATH or fall back to uvx for Python CLI tools."""
    found = shutil.which(tool)
    if found:
        return [found]
    if tool in {"ruff", "lizard"}:
        uvx = shutil.which("uvx")
        if uvx:
            return [uvx, tool]
    return None


def load_compile_db(compile_db_path: Path, repo_root: Path) -> dict[str, dict]:
    """Load compile_commands.json indexed by repo-relative normalized source path."""
    if not compile_db_path.is_file():
        return {}
    raw = json.loads(compile_db_path.read_text(encoding="utf-8"))
    by_path: dict[str, dict] = {}
    for entry in raw:
        rel = norm_rel(entry.get("file", ""), repo_root)
        by_path[rel] = entry
    return by_path


def find_target_tus(changed_files: list[str], compile_db: dict[str, dict]) -> list[str]:
    """Map changed C/C++ files (including headers) to translation units in compile_commands.json."""
    tus: list[str] = []
    seen: set[str] = set()
    by_stem: dict[str, list[str]] = defaultdict(list)
    by_dir: dict[str, list[str]] = defaultdict(list)
    for tu in sorted(compile_db):
        p = Path(tu)
        by_stem[f"{p.parent.as_posix()}/{p.stem}"].append(tu)
        by_dir[p.parent.as_posix()].append(tu)

    for rel in changed_files:
        if rel in compile_db and rel not in seen:
            seen.add(rel)
            tus.append(rel)
            continue
        p = Path(rel)
        if p.suffix not in C_CPP_HEADER_EXTS:
            continue
        matches = by_stem.get(f"{p.parent.as_posix()}/{p.stem}", [])
        if not matches:
            matches = by_dir.get(p.parent.as_posix(), [])[:2]
        for tu in matches:
            if tu not in seen:
                seen.add(tu)
                tus.append(tu)
    return tus


def _split_clang_tidy_rest(rest: str) -> tuple[str, str]:
    """Split trailing `[check-name]` suffix from a clang-tidy diagnostic message."""
    if rest.endswith("]"):
        idx = rest.rfind(" [")
        if idx != -1:
            rule = rest[idx + 2 : -1]
            if rule and "[" not in rule and "]" not in rule:
                return rest[:idx].rstrip(), rule
    return rest, "clang-tidy"


def parse_clang_tidy_output(output: str, repo_root: Path, changed_lines: dict[str, set[int]]) -> list[Finding]:
    """Filter clang-tidy warnings/errors strictly to changed lines in changed files."""
    findings: list[Finding] = []
    seen: set[tuple[str, int, str, str]] = set()
    for raw_line in output.splitlines():
        match = CLANG_TIDY_RE.match(raw_line.strip())
        if not match:
            continue
        file_str, line_str, _col, severity, rest = match.groups()
        rel = norm_rel(file_str, repo_root)
        line_no = int(line_str)
        if rel not in changed_lines or line_no not in changed_lines[rel]:
            continue
        msg, rule_name = _split_clang_tidy_rest(rest)
        key = (rel, line_no, rule_name, msg)
        if key in seen:
            continue
        seen.add(key)
        findings.append(Finding("clang-tidy", rel, line_no, severity, rule_name, msg))
    return findings


def run_clang_tidy_check(
    repo_root: Path,
    changed_lines: dict[str, set[int]],
    compile_db_path: Path | None,
    strict_tools: bool,
) -> tuple[list[Finding], list[str]]:
    c_files = sorted(f for f in changed_lines if Path(f).suffix in C_CPP_EXTS)
    if not c_files:
        return [], ["clang-tidy: skipped (no changed C/C++ files)"]
    if compile_db_path is None or not compile_db_path.is_file():
        note = "clang-tidy: skipped (no compile_commands.json provided)"
        return ([Finding("clang-tidy", ".", 1, "error", "missing-compile-db", note)] if strict_tools else [], [note])

    cmd_base = resolve_tool_cmd("clang-tidy")
    if not cmd_base:
        note = "clang-tidy: skipped (tool not installed)"
        return ([Finding("clang-tidy", ".", 1, "error", "missing-tool", note)] if strict_tools else [], [note])

    compile_db = load_compile_db(compile_db_path, repo_root)
    tus = find_target_tus(c_files, compile_db)
    if not tus:
        return [], ["clang-tidy: skipped (changed headers/files have no matching compile_commands TU)"]

    config_file = repo_root / ".clang-tidy"
    findings: list[Finding] = []
    build_dir = compile_db_path.parent
    for tu in tus:
        args = [
            *cmd_base,
            f"--config-file={config_file}",
            "-p",
            str(build_dir),
            "--extra-arg=-Wno-unknown-warning-option",
            str(repo_root / tu),
        ]
        res = subprocess.run(args, capture_output=True, text=True, errors="replace", cwd=repo_root)
        findings.extend(parse_clang_tidy_output(res.stdout + "\n" + res.stderr, repo_root, changed_lines))
    return findings, [f"clang-tidy: checked {len(tus)} TU(s) -> {len(findings)} finding(s)"]


def extract_cppcheck_flags(entry: dict, repo_root: Path) -> list[str]:
    """Extract include paths, defines, and language standard for cppcheck from compile_commands."""
    if "arguments" in entry and isinstance(entry["arguments"], list):
        tokens = [str(t) for t in entry["arguments"]]
    else:
        tokens = shlex.split(entry.get("command", ""))
    raw_dir = entry.get("directory")
    work_dir = str(raw_dir) if isinstance(raw_dir, str) and raw_dir else str(repo_root)
    flags: list[str] = []
    i = 0
    while i < len(tokens):
        tok = tokens[i]
        if tok in {"-I", "-isystem", "-D", "-U"} and i + 1 < len(tokens):
            val = tokens[i + 1]
            if tok in {"-I", "-isystem"}:
                val = os.path.normpath(os.path.join(work_dir, val))
                flags.extend(["-I", val])
            else:
                flags.extend([tok, val])
            i += 2
            continue
        if tok.startswith(("-I", "-isystem")):
            raw_path = tok[2:] if tok.startswith("-I") else tok[8:]
            if raw_path:
                flags.extend(["-I", os.path.normpath(os.path.join(work_dir, raw_path))])
        elif tok.startswith(("-D", "-U")):
            flags.append(tok)
        elif tok.startswith("-std="):
            std = tok[5:].replace("gnu++", "c++").replace("gnu", "c")
            flags.append(f"--std={std}")
        i += 1
    return flags


def parse_cppcheck_output(output: str, repo_root: Path, changed_lines: dict[str, set[int]]) -> list[Finding]:
    """Filter cppcheck diagnostics to changed lines in changed files."""
    findings: list[Finding] = []
    seen: set[tuple[str, int, str, str]] = set()
    for raw_line in output.splitlines():
        match = CPPCHECK_RE.match(raw_line.strip())
        if not match:
            continue
        file_str, line_str, severity, rule_id, msg = match.groups()
        line_no = int(line_str)
        if line_no <= 0:
            continue
        rel = norm_rel(file_str, repo_root)
        if rel not in changed_lines:
            continue
        if line_no not in changed_lines[rel] and severity != "error":
            continue
        key = (rel, line_no, rule_id, msg)
        if key in seen:
            continue
        seen.add(key)
        findings.append(Finding("cppcheck", rel, line_no, severity, rule_id, msg))
    return findings


def run_cppcheck_check(
    repo_root: Path,
    changed_lines: dict[str, set[int]],
    compile_db_path: Path | None,
    strict_tools: bool,
) -> tuple[list[Finding], list[str]]:
    c_files = sorted(f for f in changed_lines if Path(f).suffix in C_CPP_EXTS)
    if not c_files:
        return [], ["cppcheck: skipped (no changed C/C++ files)"]
    cmd_base = resolve_tool_cmd("cppcheck")
    if not cmd_base:
        note = "cppcheck: skipped (tool not installed)"
        return ([Finding("cppcheck", ".", 1, "error", "missing-tool", note)] if strict_tools else [], [note])

    compile_db = load_compile_db(compile_db_path, repo_root) if compile_db_path else {}
    tus = find_target_tus(c_files, compile_db) if compile_db else [f for f in c_files if Path(f).suffix in C_CPP_SOURCE_EXTS]
    if not tus:
        tus = c_files

    findings: list[Finding] = []
    for target in tus:
        extra_flags = extract_cppcheck_flags(compile_db[target], repo_root) if target in compile_db else []
        lang = "c" if Path(target).suffix == ".c" else "c++"
        args = [
            *cmd_base,
            "--enable=warning,performance,portability",
            "--inline-suppr",
            "--suppress=missingIncludeSystem",
            "--suppress=unmatchedSuppression",
            "--suppress=normalCheckLevelMaxBranches",
            f"--language={lang}",
            "--template={file}:{line}:{severity}:{id}:{message}",
            "--quiet",
            *extra_flags,
            str(repo_root / target),
        ]
        res = subprocess.run(args, capture_output=True, text=True, errors="replace", cwd=repo_root)
        findings.extend(parse_cppcheck_output(res.stdout + "\n" + res.stderr, repo_root, changed_lines))
    return findings, [f"cppcheck: checked {len(tus)} file(s) -> {len(findings)} finding(s)"]


def parse_lizard_csv(csv_text: str) -> dict[str, list[FuncMetric]]:
    """Parse `lizard --csv` rows keyed by raw file path."""
    by_file: dict[str, list[FuncMetric]] = defaultdict(list)
    reader = csv.reader(io.StringIO(csv_text))
    for row in reader:
        if len(row) < 11:
            continue
        try:
            nloc = int(row[0])
            ccn = int(row[1])
            params = int(row[3])
            start_line = int(row[9])
            end_line = int(row[10])
        except ValueError:
            continue
        file_path = row[6].replace("\\", "/")
        name = row[7]
        by_file[file_path].append(FuncMetric(name, params, nloc, ccn, start_line, end_line))
    return dict(by_file)


def _overlaps(func: FuncMetric, lines: set[int]) -> bool:
    return any(func.start_line <= ln <= func.end_line for ln in lines)


def evaluate_complexity_ratchet(
    rel_path: str,
    cur_funcs: list[FuncMetric],
    base_funcs: list[FuncMetric] | None,
    lines: set[int],
    limits: QualityLimits,
) -> list[Finding]:
    """Enforce complexity/size limits on newly added or modified functions."""
    base_by_name = {f.name: f for f in base_funcs} if base_funcs is not None else {}
    findings: list[Finding] = []
    for fn in cur_funcs:
        if not _overlaps(fn, lines):
            continue
        base = base_by_name.get(fn.name)
        if base is None:
            if fn.ccn > limits.ccn_max:
                findings.append(Finding("lizard", rel_path, fn.start_line, "error", "ccn", f"function '{fn.name}' CCN {fn.ccn} > {limits.ccn_max}"))
            if fn.nloc > limits.nloc_max:
                findings.append(Finding("lizard", rel_path, fn.start_line, "error", "nloc", f"function '{fn.name}' NLOC {fn.nloc} > {limits.nloc_max}"))
            if fn.params > limits.params_max:
                findings.append(Finding("lizard", rel_path, fn.start_line, "error", "params", f"function '{fn.name}' params {fn.params} > {limits.params_max}"))
            continue
        if fn.ccn > limits.ccn_max and fn.ccn > base.ccn:
            findings.append(Finding("lizard", rel_path, fn.start_line, "error", "ccn-regression", f"function '{fn.name}' CCN worsened {base.ccn} -> {fn.ccn} (max {limits.ccn_max})"))
        if fn.nloc > limits.nloc_max and fn.nloc > base.nloc + 5:
            findings.append(Finding("lizard", rel_path, fn.start_line, "error", "nloc-regression", f"function '{fn.name}' NLOC grew {base.nloc} -> {fn.nloc} (max {limits.nloc_max})"))
        if fn.params > limits.params_max and fn.params > base.params:
            findings.append(Finding("lizard", rel_path, fn.start_line, "error", "params-regression", f"function '{fn.name}' params grew {base.params} -> {fn.params} (max {limits.params_max})"))
    return findings


def _load_base_metrics(
    lizard_cmd: list[str],
    repo_root: Path,
    base_ref: str,
    rel_paths: list[str],
) -> dict[str, list[FuncMetric]]:
    base_metrics: dict[str, list[FuncMetric]] = {}
    with tempfile.TemporaryDirectory(prefix="qray_lizard_") as tmp:
        tmp_dir = Path(tmp)
        tmp_to_rel: dict[str, str] = {}
        for idx, rel in enumerate(rel_paths):
            show = _run_git(repo_root, ["show", f"{base_ref}:{rel}"], check=False)
            if show.returncode != 0:
                continue
            tmp_file = tmp_dir / f"{idx}{Path(rel).suffix}"
            tmp_file.write_text(show.stdout, encoding="utf-8")
            tmp_to_rel[tmp_file.resolve().as_posix()] = rel
        if not tmp_to_rel:
            return {}
        res = subprocess.run([*lizard_cmd, "--csv", *tmp_to_rel], capture_output=True, text=True, errors="replace")
        for raw_file, funcs in parse_lizard_csv(res.stdout).items():
            resolved = Path(raw_file).resolve().as_posix()
            if resolved in tmp_to_rel:
                base_metrics[tmp_to_rel[resolved]] = funcs
    return base_metrics


def run_lizard_check(
    repo_root: Path,
    changed_lines: dict[str, set[int]],
    base_ref: str,
    local: bool,
    limits: QualityLimits,
    strict_tools: bool,
) -> tuple[list[Finding], list[str]]:
    targets = sorted(f for f in changed_lines if Path(f).suffix in (C_CPP_EXTS | PY_EXTS))
    if not targets:
        return [], ["lizard: skipped (no changed C/C++/Python files)"]
    lizard_cmd = resolve_tool_cmd("lizard")
    if not lizard_cmd:
        note = "lizard: skipped (tool not installed)"
        return ([Finding("lizard", ".", 1, "error", "missing-tool", note)] if strict_tools else [], [note])

    res = subprocess.run([*lizard_cmd, "--csv", *[str(repo_root / t) for t in targets]], capture_output=True, text=True, errors="replace")
    raw_cur = parse_lizard_csv(res.stdout)
    cur_by_rel = {norm_rel(path, repo_root): funcs for path, funcs in raw_cur.items()}
    git_base = "HEAD" if local else base_ref
    base_by_rel = _load_base_metrics(lizard_cmd, repo_root, git_base, targets)

    findings: list[Finding] = []
    for rel in targets:
        findings.extend(
            evaluate_complexity_ratchet(rel, cur_by_rel.get(rel, []), base_by_rel.get(rel), changed_lines[rel], limits)
        )
    return findings, [f"lizard: checked {len(targets)} file(s) -> {len(findings)} finding(s)"]


def parse_ruff_output(output: str, repo_root: Path, changed_lines: dict[str, set[int]]) -> list[Finding]:
    """Filter ruff diagnostics to changed Python lines (always fail syntax/undefined names)."""
    findings: list[Finding] = []
    for raw_line in output.splitlines():
        match = RUFF_RE.match(raw_line.strip())
        if not match:
            continue
        file_str, line_str, code, msg = match.groups()
        line_no = int(line_str)
        rel = norm_rel(file_str, repo_root)
        if rel not in changed_lines:
            continue
        if line_no in changed_lines[rel] or code.startswith("E9") or code == "F821":
            findings.append(Finding("ruff", rel, line_no, "error", code, msg))
    return findings


def run_ruff_check(
    repo_root: Path,
    changed_lines: dict[str, set[int]],
    strict_tools: bool,
) -> tuple[list[Finding], list[str]]:
    py_files = sorted(f for f in changed_lines if Path(f).suffix in PY_EXTS)
    if not py_files:
        return [], ["ruff: skipped (no changed Python files)"]
    ruff_cmd = resolve_tool_cmd("ruff")
    if not ruff_cmd:
        note = "ruff: skipped (tool not installed)"
        return ([Finding("ruff", ".", 1, "error", "missing-tool", note)] if strict_tools else [], [note])

    args = [
        *ruff_cmd,
        "check",
        "--no-cache",
        "--output-format=concise",
        "--select=E4,E7,E9,F,B",
        *[str(repo_root / f) for f in py_files],
    ]
    res = subprocess.run(args, capture_output=True, text=True, errors="replace", cwd=repo_root)
    findings = parse_ruff_output(res.stdout + "\n" + res.stderr, repo_root, changed_lines)
    return findings, [f"ruff: checked {len(py_files)} file(s) -> {len(findings)} finding(s)"]


def run_shader_check(repo_root: Path) -> tuple[list[Finding], list[str]]:
    shader_dir = repo_root / "renderer/Source/Shaders"
    matrix_script = shader_dir / "CheckMatrixReads.py"
    if not matrix_script.is_file():
        return [], ["shaders: skipped (no renderer/Source/Shaders/CheckMatrixReads.py)"]
    findings: list[Finding] = []
    res = subprocess.run([sys.executable, "-B", "CheckMatrixReads.py"], capture_output=True, text=True, errors="replace", cwd=shader_dir)
    if res.returncode != 0:
        findings.append(Finding("CheckMatrixReads", "renderer/Source/Shaders/CheckMatrixReads.py", 1, "error", "matrix-read", res.stdout.strip() or res.stderr.strip()))
    gen_script = shader_dir / "GenerateShaders.py"
    dxc = shutil.which("dxc")
    if gen_script.is_file() and dxc:
        gen = subprocess.run([sys.executable, "-B", "GenerateShaders.py", "-rebuild"], capture_output=True, text=True, errors="replace", cwd=shader_dir)
        combined = gen.stdout + "\n" + gen.stderr
        if gen.returncode != 0 or re.search(r"shader builds? failed", combined):
            findings.append(Finding("dxc", "renderer/Source/Shaders/GenerateShaders.py", 1, "error", "shader-build", combined.strip()[-400:]))
        status = f"shaders: CheckMatrixReads + dxc rebuild -> {len(findings)} finding(s)"
    else:
        status = f"shaders: CheckMatrixReads (dxc not on PATH) -> {len(findings)} finding(s)"
    return findings, [status]


def format_finding(f: Finding) -> str:
    prefix = f"::{f.severity} file={f.path},line={f.line}::" if os.environ.get("GITHUB_ACTIONS") == "true" else ""
    return f"{prefix}[{f.tool}] {f.path}:{f.line} ({f.rule}): {f.message}"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Run diff-gated quality checks for QuakeRay.")
    parser.add_argument("--base", default=DEFAULT_BASE, help="Git base ref for PR diff comparison")
    parser.add_argument("--local", action="store_true", help="Check uncommitted local workspace changes instead of git diff base...HEAD")
    parser.add_argument("--compile-commands", type=Path, default=None, help="Path to CMake compile_commands.json")
    parser.add_argument("--strict-tools", action="store_true", help="Fail if any configured analysis tool or compile DB is missing")
    parser.add_argument("--skip", default="", help="Comma-separated checks to skip (clang-tidy,cppcheck,lizard,ruff,shaders)")
    parser.add_argument("--shaders", action="store_true", help="Run HLSL matrix read & DXC build checks")
    parser.add_argument("--ccn-max", type=int, default=15, help="Maximum cyclomatic complexity for new/edited functions")
    parser.add_argument("--nloc-max", type=int, default=120, help="Maximum function NLOC for new/edited functions")
    parser.add_argument("--params-max", type=int, default=7, help="Maximum function parameter count")
    args = parser.parse_args(argv)

    repo_root = Path(__file__).resolve().parents[1]
    skipped = {s.strip().lower() for s in args.skip.split(",") if s.strip()}
    compile_db = args.compile_commands
    if compile_db is None and (repo_root / "build/quality/compile_commands.json").is_file():
        compile_db = repo_root / "build/quality/compile_commands.json"

    changed_lines = collect_changed_lines(repo_root, args.base, args.local)
    all_findings: list[Finding] = []
    summaries: list[str] = []

    if changed_lines:
        limits = QualityLimits(args.ccn_max, args.nloc_max, args.params_max)
        checks = [
            ("clang-tidy", lambda: run_clang_tidy_check(repo_root, changed_lines, compile_db, args.strict_tools)),
            ("cppcheck", lambda: run_cppcheck_check(repo_root, changed_lines, compile_db, args.strict_tools)),
            ("lizard", lambda: run_lizard_check(repo_root, changed_lines, args.base, args.local, limits, args.strict_tools)),
            ("ruff", lambda: run_ruff_check(repo_root, changed_lines, args.strict_tools)),
        ]
        for name, runner in checks:
            if name in skipped:
                summaries.append(f"{name}: skipped by --skip")
                continue
            found, msgs = runner()
            all_findings.extend(found)
            summaries.extend(msgs)
    else:
        summaries.append("diff: no changed non-excluded source files")

    if args.shaders and "shaders" not in skipped:
        found, msgs = run_shader_check(repo_root)
        all_findings.extend(found)
        summaries.extend(msgs)

    for line in summaries:
        print(line)
    for finding in all_findings:
        print(format_finding(finding))
    return 1 if all_findings else 0


if __name__ == "__main__":
    raise SystemExit(main())
