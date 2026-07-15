# OBS Live Delay Plugin

## Beta

This is an early beta of an OBS plugin for adding a delay after you are already live. While the delay is building or changing, OBS shows a holding scene. Once the delayed feed is ready, it switches back, giving you time to adjust the delay without taking the stream offline.

The dock, hotkeys, holding-scene controls, packet buffer, delay controller, FLV muxer, bounded sender queue, and RTMP connection layer are implemented. The plugin also builds against OBS 32.

The transport has published synthetic H.264/AAC FLV tags to a local RTMP server, but the complete OBS encoder-to-server path still needs end-to-end A/V validation. This beta remains for development and testing, not live production use.

## What is here

- Set or change the target delay from the dock
- Return to live and clear the delay buffer
- Switch to a holding scene while the delay is building
- Use hotkeys for delay, return live, and emergency dump
- Keep delayed playback on a video keyframe
- Stop safely when packet timestamps are invalid or the buffer reaches its limit
- Mux released H.264/AAC packets into FLV on a normalized timeline
- Send on a bounded worker-thread queue with reconnect and keyframe realignment

## Build it

You need an OBS development build, Qt 6, CMake, and the Microsoft C++ Build Tools.

To run the fast core tests without OBS or Qt:

```powershell
cmake -S . -B build-core -DACTIVE_DELAY_BUILD_PLUGIN=OFF -DACTIVE_DELAY_BUILD_TESTS=ON
cmake --build build-core --config Release
ctest --test-dir build-core -C Release --output-on-failure
```

Building the DLL also needs the OBS development libraries available through `CMAKE_PREFIX_PATH`.

## Status

The RTMP/FLV path is implemented and its connection layer has been exercised against MediaMTX. Before live use, the plugin still needs an OBS end-to-end test covering A/V playback, synchronization, reconnects, delay changes, emergency dump, and shutdown.
