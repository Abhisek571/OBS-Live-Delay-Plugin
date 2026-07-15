# OBS Live Delay Plugin

## Beta

This is an early beta of an OBS plugin for adding a delay after you are already live. While the delay is building or changing, OBS shows a holding scene. Once the delayed feed is ready, it switches back, giving you time to adjust the delay without taking the stream offline.

The dock, hotkeys, holding-scene controls, packet buffer, and delay controller are working. The plugin also builds against OBS 32.

It does not send a stream yet. RTMP/FLV delivery is still being built, so this beta is for development and testing—not live production use.

## What is here

- Set or change the target delay from the dock
- Return to live and clear the delay buffer
- Switch to a holding scene while the delay is building
- Use hotkeys for delay, return live, and emergency dump
- Keep delayed playback on a video keyframe
- Stop safely when packet timestamps are invalid or the buffer reaches its limit

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

The next piece of work is RTMP/FLV muxing and a sender thread. Until that is in place, the plugin deliberately refuses to start an output rather than pretending to stream.
