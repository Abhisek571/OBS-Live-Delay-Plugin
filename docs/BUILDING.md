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

## Full plugin build

The local configured build directory is `build-plugin-clean`. CMake is not on
the default shell PATH in the maintained Windows environment:

```powershell
& "C:\Qt\Tools\CMake_64\bin\cmake.exe" --build build-plugin-clean --config Release
& "C:\Qt\Tools\CMake_64\bin\ctest.exe" --test-dir build-plugin-clean -C Release --output-on-failure
```

The full Release suite contains five CTest targets, including an OBS-linked
packet-conversion test. The built DLL is
`build-plugin-clean/Release/obs-active-live-delay.dll`.
