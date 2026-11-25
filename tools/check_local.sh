#!/usr/bin/env bash
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

# Fast local verification script for QuakeRay (incremental build + CPU tests + quality gate).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

if [[ -f /home/user/Soft/qray-env.sh ]]; then
    # shellcheck disable=SC1091
    . /home/user/Soft/qray-env.sh
fi

usage() {
    cat <<'EOF'
Usage: tools/check_local.sh [options]

Options:
  --local        Check uncommitted workspace changes against HEAD
  --base <ref>   Check git diff <ref>...HEAD (default: origin/master)
  --asan         Build & test with Clang ASan + UBSan into build/local-quality-asan
  --shaders      Force HLSL matrix-read and DXC compilation checks
  --full         Force all Python test suites + HLSL shader checks
  --no-build     Skip build_linux.sh and ctest
  --jobs <N>     Max parallel compile jobs (default: QR_JOBS or 6)
  -h, --help     Show this help message
EOF
}

mode=""
base_ref="origin/master"
use_asan=0
run_shaders=0
run_full=0
no_build=0
jobs="${QR_JOBS:-6}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --local)
            mode="local"
            shift
            ;;
        --base)
            if [[ $# -lt 2 ]]; then
                echo "error: --base requires a git ref argument" >&2
                exit 2
            fi
            mode="base"
            base_ref="$2"
            shift 2
            ;;
        --asan)
            use_asan=1
            shift
            ;;
        --shaders)
            run_shaders=1
            shift
            ;;
        --full)
            run_full=1
            run_shaders=1
            shift
            ;;
        --no-build)
            no_build=1
            shift
            ;;
        --jobs)
            if [[ $# -lt 2 ]]; then
                echo "error: --jobs requires a number" >&2
                exit 2
            fi
            jobs="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "error: unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ -z "$mode" ]]; then
    if [[ -n "$(git status --porcelain --untracked-files=normal)" ]]; then
        mode="local"
    else
        mode="base"
    fi
fi

start_ts="$(date +%s)"

if [[ "$use_asan" -eq 1 ]]; then
    build_dir="$repo_root/build/local-quality-asan"
    sanitizer_flag="ON"
else
    build_dir="$repo_root/build/local-quality"
    sanitizer_flag="OFF"
fi

if [[ "$no_build" -eq 0 ]]; then
    cc_bin="${CC:-$(command -v clang || command -v gcc || true)}"
    cxx_bin="${CXX:-$(command -v clang++ || command -v g++ || true)}"
    if [[ -z "$cc_bin" || -z "$cxx_bin" ]]; then
        echo "error: no C/C++ compiler found on PATH" >&2
        exit 1
    fi

    if command -v ccache >/dev/null 2>&1 && [[ "${QR_NO_CCACHE:-0}" != "1" ]]; then
        export CCACHE_SLOPPINESS="${CCACHE_SLOPPINESS:-pch_defines,time_macros,locale}"
        ccache -z >/dev/null 2>&1 || true
    fi

    cmake_extra_args=(
        -G Ninja
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
        "-DQR_ENABLE_SANITIZERS=$sanitizer_flag"
        -DUSE_CODEC_MP3=OFF
        -DUSE_CODEC_VORBIS=OFF
        -DUSE_CODEC_FLAC=OFF
        -DUSE_CODEC_OPUS=OFF
        -DUSE_CODEC_XMP=OFF
        -DUSE_CODEC_MIKMOD=OFF
    )
    if [[ -n "${QR_LINUX_SYSROOT:-}" ]]; then
        cmake_extra_args+=("-DCMAKE_PREFIX_PATH=$QR_LINUX_SYSROOT")
    fi

    build_env=(
        "CC=$cc_bin"
        "CXX=$cxx_bin"
        "QR_BUILD_DIR=$build_dir"
        "QR_JOBS=$jobs"
        "QR_BUILD_TESTS=0"
        "QR_BUILD_AUDIO_TESTS=${QR_BUILD_AUDIO_TESTS:-1}"
        "QR_SKIP_ASSETS=1"
        "QR_RUN_TESTS=0"
    )

    if command -v nice >/dev/null 2>&1; then
        env "${build_env[@]}" nice -n 15 "$repo_root/build_linux.sh" "${cmake_extra_args[@]}"
    else
        env "${build_env[@]}" "$repo_root/build_linux.sh" "${cmake_extra_args[@]}"
    fi

    if [[ "$use_asan" -eq 1 ]]; then
        ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
        ctest --test-dir "$build_dir" --output-on-failure -LE gpu
    else
        ctest --test-dir "$build_dir" --output-on-failure -LE gpu
    fi
fi

if [[ "$mode" == "local" ]]; then
    changed_files="$(git diff --name-only HEAD; git ls-files --others --exclude-standard)"
else
    changed_files="$(git diff --name-only "${base_ref}...HEAD")"
fi

run_py_tests="$run_full"
if [[ "$run_py_tests" -eq 0 ]] && printf '%s\n' "$changed_files" | grep -qE '\.py$'; then
    run_py_tests=1
fi
if [[ "$run_py_tests" -eq 1 ]]; then
    python3 -B -m unittest discover -s "$repo_root/tools" -p "test_*.py" -v
    python3 -B -m unittest discover -s "$repo_root/tests/perf" -p "test_*.py" -v
    if [[ -d "$repo_root/tools/quakeray_mcp/tests" ]] && command -v uv >/dev/null 2>&1; then
        PYTHONPATH="$repo_root/tools/quakeray_mcp/src" uv run --no-build --no-sync --offline --frozen --python 3.10 --project "$repo_root/tools/quakeray_mcp" python -B -m unittest discover -s "$repo_root/tools/quakeray_mcp/tests" -p "test_*.py" -q
    fi
fi

if [[ "$run_shaders" -eq 0 ]] && printf '%s\n' "$changed_files" | grep -qE '^renderer/Source/Shaders/.*\.(hlsl|hlsli)$'; then
    run_shaders=1
fi
if [[ "$run_shaders" -eq 1 ]] && [[ -d "$repo_root/renderer/Source/Shaders" ]]; then
    (
        cd "$repo_root/renderer/Source/Shaders"
        python3 -B CheckMatrixReads.py
        log="$(python3 -B GenerateShaders.py -rebuild)"
        printf '%s\n' "$log"
        if printf '%s\n' "$log" | grep -qE 'shader builds? failed'; then
            echo "error: GenerateShaders.py reported shader compilation failure" >&2
            exit 1
        fi
    )
fi

quality_args=(--strict-tools)
if [[ "$mode" == "local" ]]; then
    quality_args+=(--local)
else
    quality_args+=(--base "$base_ref")
fi

compile_db=""
for candidate in \
    "$build_dir/compile_commands.json" \
    "$repo_root/build/local-quality/compile_commands.json" \
    "$repo_root/build/quality/compile_commands.json" \
    "$repo_root/build/linux-debug/compile_commands.json"; do
    if [[ -f "$candidate" ]]; then
        compile_db="$candidate"
        break
    fi
done
if [[ -n "$compile_db" ]]; then
    quality_args+=(--compile-commands "$compile_db")
fi

python3 -B "$repo_root/tools/check_quality.py" "${quality_args[@]}"

if command -v ccache >/dev/null 2>&1; then
    ccache -s
fi

elapsed=$(( $(date +%s) - start_ts ))
echo "Local QuakeRay checks passed in ${elapsed}s"
