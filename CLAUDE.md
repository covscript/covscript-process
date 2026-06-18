# CLAUDE.md

This file provides project guidance for Claude when working in this repository.

## Core Constraints

- Keep module name as `process`. Do not rename it.
- When running scripts/tests with `cs`, always pass `-i` to prioritize local extension loading.
- Preferred local import path for this repo is `./build/imports`.
- Cross-platform compatibility is required. Avoid platform-specific APIs unless required for core functionality.
- If platform-specific code is necessary, isolate it clearly, document differences, keep them minimal, and preserve consistent behavior.
- You may format code using `astyle -A4 -N -t` if available, but do not change existing formatting style unless necessary for readability or consistency. Prioritize correctness and functionality.

## SDK and ABI Compatibility

- Build against an SDK that matches the active runtime interpreter ABI.
- If extension import fails due to ABI mismatch, reconfigure/rebuild with the matching SDK path.
- Prefer using `csbuild/make.sh` or `csbuild/make.bat` for validation builds, since they handle SDK path and ABI consistency.
- If ABI behavior is unclear, ask the user which SDK/runtime pair they are using before changing code.

## Windows Build Preference

- On Windows, prefer MinGW-w64 for compatibility with the SDK and runtime.
- With MinGW-w64, configure with `-G "MinGW Makefiles"` and build with `mingw32-make -j4`.

## Recommended Local Commands (Windows / PowerShell)

### Configure and build

```powershell
cd cmake-build/mingw-w64
cmake -G "MinGW Makefiles" ../..
mingw32-make -j4
```

### Copy extension into import path used by `cs -i`

```powershell
Copy-Item ./cmake-build/mingw-w64/process.cse ./build/imports/process.cse -Force
```

### Run tests (must use `-i`)

```powershell
cs -i ./build/imports ./tests/test_unit.csc
cs -i ./build/imports ./tests/test_async.csc
cs -i ./build/imports ./tests/test_file_redirect.csc
cs -i ./build/imports ./tests/test_stream.csc
cs -i ./build/imports ./tests/test_fiber.csc
cs -i ./build/imports ./tests/test_corner.csc
```

## Implementation Notes

- If runtime behavior differs from C++ source expectations, verify which module is loaded first before changing APIs.
- Prefer minimal, targeted changes; do not rewrite unrelated docs or tests.
- Some CMake settings are maintained by SDK `csbuild.cmake` and may change with SDK updates. Avoid duplicating or overriding them unless necessary.
- C++ standard is currently C++17, as set by SDK `csbuild.cmake`. Do not change this unless required for SDK/runtime compatibility.

## API Surface Guidance

- `mpp` C++ library and CovScript CNI surface do not need to be identical.
- `mpp` keeps C++-oriented ergonomics (for example `shell(nullptr_t)`, `wait_timeout_ms()`, `begin_wait()`/`poll_wait()`/`collect_wait()`, method chaining).
- CNI should expose only what is appropriate for CovScript scripts and may omit, rename, or merge APIs.
- Do not blindly mirror every `mpp` method into CNI.
