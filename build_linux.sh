#!/usr/bin/env bash
#
# Linux Debug build for QuakeRay.
#
# This is the Linux counterpart of build_win.ps1 for the parts that matter to
# FSR: the FidelityFX 1.1.4 Vulkan provider is built from the vendored SDK
# sources first, then the engine is configured against that output. No
# checked-in provider binary is used.
#
# Common environment overrides:
#   QR_BUILD_DIR          build directory (default: build/linux-debug)
#   QR_FFX_SDK_ROOT       FidelityFX SDK 1.1.4 source root
#   GLSLANG_VALIDATOR     glslangValidator executable
#   VULKAN_HEADERS_DIR    directory containing vulkan/vulkan.h
#   QR_DXC                dxc compiler executable (needed for shaders & QR_BUILD_TESTS=1)
#   QR_BUILD_TESTS        1 to build renderer GPU regression tests (-DQR_BUILD_TESTS=ON)
#   QR_BUILD_AUDIO_TESTS  1 to build audio_regression test (-DBUILD_AUDIO_TESTS=ON)
#   QR_SKIP_FFX_BUILD     1 to reuse existing libamd_fidelityfx_vk.so when present
#   QR_JOBS               parallel build job count (default: min(nproc, RAM / 4 GiB))
#   QR_NO_CCACHE          1 to disable automatic ccache compiler launcher
#   QR_REBUILD_SHADERS    1 to pass -rebuild to GenerateShaders.py
#   QR_SKIP_ASSETS        1 to skip shader/pkz packing
#   QR_ID1_DIR            optional source directory to stage id1/pak0.pak & pak1.pak
#   QR_AD_DIR             optional source Arcane Dimensions directory
#   QR_AD_SAVES_DIR       optional source directory for canonical AD .sav files
#   QR_AD_MATERIALS_DIR   optional source directory for AD qray.materials.yaml / lights
#   QR_RUN_TESTS          1 to run ctest after the build (default: 0). The CPU
#                         suite is built by default; the GPU regression suite is
#                         enabled with QR_BUILD_TESTS=1 (and needs dxc).
#
# Extra arguments are passed to the CMake configure step, e.g.
#   ./build_linux.sh -DCMAKE_PREFIX_PATH=/path/to/sysroot/usr
#
set -euo pipefail

compute_default_jobs() {
    local cpus mem_kb mem_jobs jobs
    cpus="$(nproc)"
    if [[ -r /proc/meminfo ]]; then
        mem_kb="$(awk '/^MemAvailable:/ {print $2; exit}' /proc/meminfo)"
        if [[ -z "$mem_kb" ]]; then
            mem_kb="$(awk '/^MemTotal:/ {print $2; exit}' /proc/meminfo)"
        fi
        mem_jobs=$(( mem_kb / 4194304 ))
        if [[ "$mem_jobs" -lt 1 ]]; then
            mem_jobs=1
        fi
        jobs="$cpus"
        if [[ "$mem_jobs" -lt "$jobs" ]]; then
            jobs="$mem_jobs"
        fi
    else
        jobs="$cpus"
    fi
    if [[ "$jobs" -lt 1 ]]; then
        jobs=1
    fi
    echo "$jobs"
}

anchor_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$anchor_root"

# NVRHI's stock c_MaxBindingLayouts is below what the direct/indirect RT
# pipelines need. build_win.ps1 applies the pinned patch for the duration of
# the build; mirror that here so a fresh Linux checkout is not left with a
# binary whose RT pass is unavailable.
nvrhi_dir="$anchor_root/third_party/nvrhi"
nvrhi_patch="$anchor_root/third_party/nvrhi-max-binding-layouts.patch"
nvrhi_patched_here=0
cleanup_nvrhi() {
    if [[ "$nvrhi_patched_here" == "1" ]]; then
        git -C "$nvrhi_dir" apply --reverse "$nvrhi_patch" >/dev/null 2>&1 || true
    fi
}
trap cleanup_nvrhi EXIT

if [[ -f "$nvrhi_patch" && -f "$nvrhi_dir/include/nvrhi/nvrhi.h" ]]; then
    if git -C "$nvrhi_dir" apply --check --reverse "$nvrhi_patch" >/dev/null 2>&1; then
        echo "NVRHI patch is already applied, leaving it in place"
    else
        if ! git -C "$nvrhi_dir" apply "$nvrhi_patch"; then
            echo "error: failed to apply $nvrhi_patch to third_party/nvrhi" >&2
            exit 1
        fi
        nvrhi_patched_here=1
        echo "Applied the NVRHI patch for this build"
    fi
fi

build_dir="${QR_BUILD_DIR:-$anchor_root/build/linux-debug}"
sdk_root="${QR_FFX_SDK_ROOT:-$anchor_root/renderer/Source/FidelityFX_SDK_1.1.4/src}"
ffx_out="$build_dir/ffx"

if [[ ! -f "$sdk_root/sdk/src/backends/vk/ffx_vk.cpp" ]]; then
    echo "error: FidelityFX SDK 1.1.4 sources were not found at $sdk_root" >&2
    echo "       set QR_FFX_SDK_ROOT to a directory containing sdk/ and ffx-api/" >&2
    exit 1
fi

if [[ -z "${GLSLANG_VALIDATOR:-}" ]]; then
    if command -v glslangValidator >/dev/null 2>&1; then
        GLSLANG_VALIDATOR="$(command -v glslangValidator)"
    elif [[ -n "${VULKAN_SDK:-}" && -x "$VULKAN_SDK/bin/glslangValidator" ]]; then
        GLSLANG_VALIDATOR="$VULKAN_SDK/bin/glslangValidator"
    fi
fi
if [[ -z "${GLSLANG_VALIDATOR:-}" ]]; then
    echo "error: glslangValidator was not found; set GLSLANG_VALIDATOR" >&2
    exit 1
fi

mkdir -p "$build_dir"
if [[ "${QR_SKIP_FFX_BUILD:-0}" == "1" && -f "$ffx_out/libamd_fidelityfx_vk.so" ]]; then
    echo "Using prebuilt FidelityFX provider at $ffx_out/libamd_fidelityfx_vk.so (QR_SKIP_FFX_BUILD=1)"
else
    QR_FFX_SDK_ROOT="$sdk_root" \
    QR_FFX_OUT_DIR="$ffx_out" \
    GLSLANG_VALIDATOR="$GLSLANG_VALIDATOR" \
    "$anchor_root/renderer/Source/FidelityFX_SDK_1.1.4/linux/build_provider.sh"
fi

dxc_bin="${QR_DXC:-}"
if [[ -z "$dxc_bin" && -n "${VULKAN_SDK:-}" && -x "$VULKAN_SDK/bin/dxc" ]]; then
    dxc_bin="$VULKAN_SDK/bin/dxc"
fi
if [[ -z "$dxc_bin" ]] && command -v dxc >/dev/null 2>&1; then
    dxc_bin="$(command -v dxc)"
fi

if [[ "${QR_BUILD_TESTS:-0}" == "1" ]] && [[ -z "$dxc_bin" || ! -x "$dxc_bin" ]]; then
    echo "error: QR_BUILD_TESTS=1 requires dxc (set QR_DXC or VULKAN_SDK)" >&2
    exit 1
fi

qr_jobs="${QR_JOBS:-$(compute_default_jobs)}"
if ! [[ "$qr_jobs" =~ ^[1-9][0-9]*$ ]]; then
    echo "error: invalid QR_JOBS value: '$qr_jobs'" >&2
    exit 1
fi

cmake_args=(
    -S "$anchor_root"
    -B "$build_dir"
    -DCMAKE_BUILD_TYPE=Debug
    -DQR_FFX_SDK_ROOT="$sdk_root"
    -DQR_FFX_INCLUDE_DIR="$sdk_root/ffx-api/include"
    -DQR_FFX_VK_SO="$ffx_out/libamd_fidelityfx_vk.so"
)

if [[ -n "${VULKAN_HEADERS_DIR:-}" ]]; then
    cmake_args+=(-DVulkan_INCLUDE_DIR="$VULKAN_HEADERS_DIR")
fi
if [[ "${QR_BUILD_TESTS:-0}" == "1" ]]; then
    cmake_args+=(-DQR_BUILD_TESTS=ON -DQR_DXC="$dxc_bin")
fi
if [[ "${QR_BUILD_AUDIO_TESTS:-0}" == "1" ]]; then
    cmake_args+=(-DBUILD_AUDIO_TESTS=ON)
fi

has_c_launcher=0
has_cxx_launcher=0
for arg in "$@"; do
    case "$arg" in
        -DCMAKE_C_COMPILER_LAUNCHER=*|-DCMAKE_C_COMPILER_LAUNCHER:*) has_c_launcher=1 ;;
        -DCMAKE_CXX_COMPILER_LAUNCHER=*|-DCMAKE_CXX_COMPILER_LAUNCHER:*) has_cxx_launcher=1 ;;
    esac
done
if [[ "${QR_NO_CCACHE:-0}" != "1" ]] && command -v ccache >/dev/null 2>&1; then
    if [[ "$has_c_launcher" -eq 0 ]]; then
        cmake_args+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache)
    fi
    if [[ "$has_cxx_launcher" -eq 0 ]]; then
        cmake_args+=(-DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
    fi
fi

cmake "${cmake_args[@]}" "$@"
cmake --build "$build_dir" --parallel "$qr_jobs"

if [[ "${QR_SKIP_ASSETS:-0}" != "1" ]]; then
    if [[ -n "$dxc_bin" && -x "$dxc_bin" ]]; then
        shader_dir="$anchor_root/renderer/Source/Shaders"
        dxc_dir="$(dirname "$dxc_bin")"
        gen_args=()
        if [[ "${QR_REBUILD_SHADERS:-0}" == "1" ]]; then
            gen_args+=(-rebuild)
        fi
        gen_log="$(PATH="$dxc_dir:$PATH" python3 -B -c "
import os, subprocess, sys
os.chdir(sys.argv[1])
p = subprocess.run([sys.executable, 'GenerateShaders.py', *sys.argv[2:]], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
print(p.stdout, end='')
sys.exit(p.returncode)
" "$shader_dir" ${gen_args[@]+"${gen_args[@]}"})"
        printf '%s\n' "$gen_log"
        if printf '%s\n' "$gen_log" | grep -qE 'shader builds? failed'; then
            echo "error: GenerateShaders.py reported shader build failures" >&2
            exit 1
        fi
    fi

    pack_args=(--repo "$anchor_root" --build-dir "$build_dir")
    if [[ "${QR_BUILD_TESTS:-0}" == "1" ]]; then
        pack_args+=(--require-shaders)
    fi
    if [[ -n "${QR_ID1_DIR:-}" ]]; then pack_args+=(--id1-dir "$QR_ID1_DIR"); fi
    if [[ -n "${QR_AD_DIR:-}" ]]; then pack_args+=(--ad-dir "$QR_AD_DIR"); fi
    if [[ -n "${QR_AD_SAVES_DIR:-}" ]]; then pack_args+=(--ad-saves-dir "$QR_AD_SAVES_DIR"); fi
    if [[ -n "${QR_AD_MATERIALS_DIR:-}" ]]; then pack_args+=(--ad-materials-dir "$QR_AD_MATERIALS_DIR"); fi
    python3 -B "$anchor_root/tools/pack_linux_assets.py" "${pack_args[@]}"
fi

if [[ "${QR_RUN_TESTS:-0}" == "1" ]]; then
    DISPLAY="${DISPLAY:-:0}" ctest --test-dir "$build_dir" --output-on-failure
fi
