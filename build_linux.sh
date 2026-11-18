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
#   QR_BUILD_DIR        build directory (default: build/linux-debug)
#   QR_FFX_SDK_ROOT     FidelityFX SDK 1.1.4 source root
#   GLSLANG_VALIDATOR   glslangValidator executable
#   VULKAN_HEADERS_DIR  directory containing vulkan/vulkan.h
#   QR_RUN_TESTS        1 to run ctest after the build (default: 0). The CPU
#                       suite is built by default; the GPU regression suite is
#                       opt-in with -DQR_BUILD_TESTS=ON (it also needs dxc).
#
# Extra arguments are passed to the CMake configure step, e.g.
#   ./build_linux.sh -DCMAKE_PREFIX_PATH=/path/to/sysroot/usr
#
set -euo pipefail

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
QR_FFX_SDK_ROOT="$sdk_root" \
QR_FFX_OUT_DIR="$ffx_out" \
GLSLANG_VALIDATOR="$GLSLANG_VALIDATOR" \
"$anchor_root/renderer/Source/FidelityFX_SDK_1.1.4/linux/build_provider.sh"

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

cmake "${cmake_args[@]}" "$@"
cmake --build "$build_dir" --parallel "$(nproc)"

if [[ "${QR_RUN_TESTS:-0}" == "1" ]]; then
    ctest --test-dir "$build_dir" --output-on-failure
fi
