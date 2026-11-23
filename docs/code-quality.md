# Code Quality and Regression Protection

Both `QuakeRay` (host engine, C11/C++/Python) and `QuakeRaySDK` (`renderer/`, C++20/HLSL) enforce automated regression protection without breaking legacy code (`Clean as You Code` / diff-gated).

## Checks Enforced

| Layer | Scope | Mechanism | Where |
| --- | --- | --- | --- |
| Static analysis (C/C++) | Bugprone, Clang Analyzer, performance, cppcheck | `clang-tidy` (`.clang-tidy`) + `cppcheck`, gated to changed lines | `tools/check_quality.py` |
| Runtime sanitizers | Memory safety & UB on CPU/audio/contract tests | Clang `ASan` + `UBSan` + `LSan` (`-DQR_ENABLE_SANITIZERS=ON`) | `CMakeLists.txt`, `ctest -LE gpu` |
| Complexity ratchet | New/edited C, C++, Python functions | `lizard` (`CCN <= 15`, `NLOC <= 120`, `params <= 7`) | `tools/check_quality.py` |
| Python linting | Bug patterns, syntax, undefined names | `ruff` (`E4,E7,E9,F,B`), gated to changed lines | `tools/check_quality.py` |
| HLSL shaders | Matrix read parity & SPIR-V compilation | `CheckMatrixReads.py` + `GenerateShaders.py -rebuild` (`dxc`) | `.github/workflows/quality-linux.yml` |

Third-party/vendored trees (`third_party/`, `Quake/mimalloc/`, `Source/FidelityFX_SDK_1.1.4/`, `Source/KTX/`, `Source/Stb/`, `Source/Vma/`, `Source/Shaders/Reflection/`) are excluded from static analysis.

## Running Locally

Run the unified fast local verification script (`tools/check_local.sh`). It sources `/home/user/Soft/qray-env.sh` when present, uses `ccache` automatically, caps build concurrency to 6 jobs (`QR_JOBS`), builds incrementally, runs CPU CTests, and executes diff-gated `tools/check_quality.py`:

```bash
./tools/check_local.sh                  # auto-selects --local (dirty tree) or --base origin/master
./tools/check_local.sh --asan           # build & test with Clang ASan + UBSan
./tools/check_local.sh --no-build       # run static analysis / unit tests without rebuilding
./tools/check_local.sh --full           # force Python unit tests + HLSL shader verification
```

For `QuakeRaySDK` (`renderer/`), use `renderer/tools/check_local.sh` with `--base origin/main` (or `--local`).

To invoke `check_quality.py` or `build_linux.sh` directly:

```bash
python3 -B tools/check_quality.py --local --compile-commands build/local-quality/compile_commands.json
python3 -B tools/check_quality.py --base origin/master --compile-commands build/local-quality/compile_commands.json --shaders
```

To build and run tests manually with Clang sanitizers and `ccache`:

```bash
CC=clang CXX=clang++ \
QR_BUILD_DIR="$PWD/build/quality" \
QR_BUILD_AUDIO_TESTS=1 \
QR_SKIP_ASSETS=1 \
./build_linux.sh -G Ninja \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DQR_ENABLE_SANITIZERS=ON \
  -DUSE_CODEC_MP3=OFF -DUSE_CODEC_VORBIS=OFF -DUSE_CODEC_FLAC=OFF \
  -DUSE_CODEC_OPUS=OFF -DUSE_CODEC_XMP=OFF -DUSE_CODEC_MIKMOD=OFF
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  ctest --test-dir build/quality --output-on-failure -LE gpu
```

## Recommended Branch Protection Checks

- **`f1ames0ff/QuakeRay` (`master`)**: require `ASan/UBSan + Static Analysis + Tests` (`quality-linux.yml`) and `Debug` / `Release` (`build-windows.yml`).
- **`f1ames0ff/QuakeRaySDK` (`main`)**: require `ASan/UBSan + Static Analysis + Shaders` (`quality-linux.yml`).
