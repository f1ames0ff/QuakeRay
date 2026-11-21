# Code Quality and Regression Protection

Both `QuakeRay` (host engine, C11/C++/Python) and `QuakeRaySDK` (`renderer/`, C++20/HLSL) enforce automated regression protection without breaking legacy code (`Clean as You Code` / diff-gated).

## Checks Enforced

| Layer | Scope | Mechanism | Where |
| --- | --- | --- | --- |
| SonarQube Cloud | C, C++, Python (`Clean as You Code`) | Official `sonar-scanner-cli` + `compile_commands.json` + Quality Gate | `sonar-project.properties`, `.github/workflows/quality-linux.yml` |
| Static analysis (C/C++) | Bugprone, Clang Analyzer, performance, cppcheck | `clang-tidy` (`.clang-tidy`) + `cppcheck`, gated to changed lines | `tools/check_quality.py` |
| Runtime sanitizers | Memory safety & UB on CPU/audio/contract tests | Clang `ASan` + `UBSan` + `LSan` (`-DQR_ENABLE_SANITIZERS=ON`) | `CMakeLists.txt`, `ctest -LE gpu` |
| Complexity ratchet | New/edited C, C++, Python functions | `lizard` (`CCN <= 15`, `NLOC <= 120`, `params <= 7`) | `tools/check_quality.py` |
| Python linting | Bug patterns, syntax, undefined names | `ruff` (`E4,E7,E9,F,B`), gated to changed lines | `tools/check_quality.py` |
| HLSL shaders | Matrix read parity & SPIR-V compilation | `CheckMatrixReads.py` + `GenerateShaders.py -rebuild` (`dxc`) | `.github/workflows/quality-linux.yml` |

Third-party/vendored trees (`third_party/`, `Quake/mimalloc/`, `Source/FidelityFX_SDK_1.1.4/`, `Source/KTX/`, `Source/Stb/`, `Source/Vma/`, `Source/Shaders/Reflection/`) are excluded from static analysis and Sonar scans.

## Running Locally

Check uncommitted workspace edits against `HEAD` (automatically discovers `clang-tidy`, `cppcheck`, `ruff`, `lizard` via `PATH` or `uvx`):

```bash
python3 -B tools/check_quality.py --local --compile-commands build/quality/compile_commands.json
```

Compare a feature branch against `origin/master` (or `origin/main` inside `renderer/`):

```bash
python3 -B tools/check_quality.py --base origin/master --compile-commands build/quality/compile_commands.json --shaders
```

Build and run tests locally with Clang sanitizers:

```bash
CC=clang CXX=clang++ cmake -S . -B build/quality -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DQR_ENABLE_SANITIZERS=ON \
  -DQR_BUILD_TESTS=ON \
  -DBUILD_AUDIO_TESTS=ON \
  -DUSE_CODEC_MP3=OFF -DUSE_CODEC_VORBIS=OFF -DUSE_CODEC_FLAC=OFF \
  -DUSE_CODEC_OPUS=OFF -DUSE_CODEC_XMP=OFF -DUSE_CODEC_MIKMOD=OFF
cmake --build build/quality --parallel 8
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  ctest --test-dir build/quality --output-on-failure -LE gpu
```

## Activating SonarQube Cloud

Both `.github/workflows/quality-linux.yml` workflows authenticate exclusively via GitHub OIDC **Secretless analysis** (no `SONAR_TOKEN` secret is read, so stale or placeholder repository secrets cannot interfere) and keep **zero marketplace `uses:` actions** (`allowed_actions: local_only`). Both `.sonarcloud.properties` (Automatic Analysis) and `sonar-project.properties` (CI scanner) are present so vendored trees and cross-repo submodule copies stay excluded.

1. In SonarQube Cloud Organization settings (**Administration -> Analysis method**), enable **Secretless analysis for GitHub Actions** (`f1ames0ff`).
2. In each project's settings (`f1ames0ff_QuakeRay` and `f1ames0ff_QuakeRaySDK`, **Administration -> Analysis Method**), turn **Automatic Analysis** **OFF** so CI-based scanning with `compile_commands.json` (`Clean as You Code`) runs without conflict.
3. Optionally set repository variables `SONAR_ORGANIZATION`, `SONAR_PROJECT_KEY`, or `SONAR_REGION` (`eu`, `us`, `au`) if they differ from the defaults (`f1ames0ff`, `f1ames0ff_QuakeRay` / `f1ames0ff_QuakeRaySDK`, `eu`).

## Recommended Branch Protection Checks

- **`f1ames0ff/QuakeRay` (`master`)**: require `ASan/UBSan + Static Analysis + Tests + Sonar` (`quality-linux.yml`) and `Release` / `Debug` (`build-windows.yml`).
- **`f1ames0ff/QuakeRaySDK` (`main`)**: require `ASan/UBSan + Static Analysis + Shaders` (`quality-linux.yml`).
