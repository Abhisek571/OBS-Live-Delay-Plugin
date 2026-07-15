# OBS Live Delay Plugin

<p align="center">
  <img src="docs/production-warning.svg" width="100%" alt="Warning: Not production-ready. Genuine delayed OBS encoder-to-RTMP playback has not been verified. Do not use this beta for a live production broadcast. Not working as of v0.1.32; see issues.">
</p>

## Beta

Version `v0.1.32` is Beta 3 of an OBS plugin for adding a delay after you are already live. While the delay is building or changing, OBS shows a holding scene. Once the delayed feed is ready, it switches back, giving you time to adjust the delay.

The dock, hotkeys, holding-scene controls, packet buffer, delay controller, FLV muxer, bounded sender queue, and RTMP connection layer are implemented. Beta 3 fixes the dock registration and shutdown lifecycle tracked in [#1](https://github.com/Abhisek571/OBS-Live-Delay-Plugin/issues/1) and [#2](https://github.com/Abhisek571/OBS-Live-Delay-Plugin/issues/2), and implements the normal-stream packet capture and delayed-output handoff tracked in [#3](https://github.com/Abhisek571/OBS-Live-Delay-Plugin/issues/3). Runtime verification and the Twitch Enhanced Broadcasting offline regression are tracked separately in [#4](https://github.com/Abhisek571/OBS-Live-Delay-Plugin/issues/4).

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
- Reopen the dock from OBS's **Docks** menu and shut down without the beta.2 dock-lifecycle crash
- Package the module's `en-US` locale file

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
