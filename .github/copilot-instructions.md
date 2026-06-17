# Copilot Instructions

## Project-specific constraints

- Keep module name as `process`. Do not rename.
- When running scripts/tests with `cs`, always pass `-i` to prioritize local extension loading.
- Prefer using the SDK that matches the active runtime interpreter ABI.
- If ABI behavior is unclear, ask the user which SDK/runtime pair they are using before changing code.
- Preferred local import path for this repo is `./build/imports`.
- Cross-platform compatibility is a key requirement; avoid platform-specific APIs unless required for core functionality.
- If platform-specific code is required, isolate it clearly, document differences, keep them minimal, and preserve consistent behavior.
- You can format codes using `astyle -A4 -N -t` if tools are available, but do not change existing formatting style unless necessary for readability or consistency. Focus on functionality and correctness first.

## Build and runtime compatibility

- Build against SDK that matches the runtime interpreter ABI.
- If extension import fails with ABI mismatch, reconfigure/rebuild with the matching SDK path above.
- Prefer using `csbuild/make.sh` or `csbuild/make.bat` for validating builds, as they handle SDK path and ABI consistency.
- In Windows, prefer MinGW-w64 for better compatibility with the SDK and runtime.
- When using MinGW-w64, configure with `-G "MinGW Makefiles"` and build with `mingw32-make -j4`.

## Recommended local commands (Windows / PowerShell)

- Configure and build:
  - `cd cmake-build/mingw-w64`
  - `cmake -G "MinGW Makefiles" ../..`
  - `mingw32-make -j4`

- Copy extension into import path used by `cs -i`:
  - `Copy-Item ./cmake-build/mingw-w64/process.cse ./build/imports/process.cse -Force`

- Run tests (must use `-i`):
  - `cs -i ./build/imports ./tests/test_unit.csc`
  - `cs -i ./build/imports ./tests/test_async.csc`
  - `cs -i ./build/imports ./tests/test_file_redirect.csc`

## Notes for future edits

- If runtime behavior differs from C++ source expectations, verify which module is loaded first before changing APIs.
- Prefer minimal, targeted changes; do not rewrite unrelated docs or tests.
- Some CMake settings are maintained by the SDK `csbuild.cmake` and may change with SDK updates; avoid duplicating or overriding them in this repo unless necessary.
- C++ standard is currently C++17, as set by the SDK's `csbuild.cmake`. Do not change this unless necessary for compatibility with the SDK or runtime.
