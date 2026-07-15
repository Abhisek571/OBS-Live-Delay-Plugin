# OBS Active Live Delay

Windows-first OBS Studio plugin for operating an RTMP stream with a live-adjustable encoded-packet delay.

## Status

This repository contains the v1 plugin foundation: the bounded packet buffer, delay state machine, dock, scene switching, hotkeys, and an OBS encoded-output adapter. The RTMP wire transport is deliberately isolated behind `RtmpTransport`; it must be connected to a supported RTMP/FLV muxer before a release can publish a stream. This prevents a partial transport implementation from silently emitting invalid RTMP.

## Development build

Install an OBS Studio development environment that exports CMake packages for `libobs` and `obs-frontend-api`, plus Qt 6 Widgets. Then configure and build:

```powershell
cmake -S . -B build -DACTIVE_DELAY_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release
```

## Intended end-user installation

Release ZIPs will preserve the OBS plugin layout. With OBS closed, extract the archive into the OBS installation directory, then open **Docks → OBS Active Live Delay**. V1 streams are started and stopped from the plugin dock rather than OBS's standard **Start Streaming** control.
