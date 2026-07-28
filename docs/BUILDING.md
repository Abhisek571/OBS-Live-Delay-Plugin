# Building and testing

## Prerequisites

- An OBS Studio development build
- Qt 6
- CMake
- Microsoft C++ Build Tools

Building the plugin DLL requires the OBS development libraries to be available
through `CMAKE_PREFIX_PATH`.

## Core tests

These dependency-free tests do not require OBS or Qt:

```powershell
cmake -S . -B build-core -DACTIVE_DELAY_BUILD_PLUGIN=OFF -DACTIVE_DELAY_BUILD_TESTS=ON
cmake --build build-core --config Release
ctest --test-dir build-core -C Release --output-on-failure
```

## Full plugin build for OBS 32.2.1

The local configured build directory is `build-plugin-obs322`. It uses the
OBS 32.2.1 source/SDK build in `third_party/obs-studio/build_x64_322`, the
official 2026-07-15 OBS dependencies, FFmpeg 8.1, and Qt 6.11. CMake is not on
the default shell PATH in the maintained Windows environment:

```powershell
& "C:\Qt\Tools\CMake_64\bin\cmake.exe" --build build-plugin-obs322 --config Release
& "C:\Qt\Tools\CMake_64\bin\ctest.exe" --test-dir build-plugin-obs322 -C Release --output-on-failure
```

The core suite contains eight CTest targets. The full Release suite contains
ten targets, including an OBS-linked packet-conversion test and a headless Qt
narrow-dock test. The built DLL is
`build-plugin-obs322/Release/obs-active-live-delay.dll`. It must import
`avformat-62.dll` and `avutil-60.dll`; the prior OBS 32.1.2 build imported
FFmpeg 7 DLLs that OBS 32.2.1 no longer ships.
