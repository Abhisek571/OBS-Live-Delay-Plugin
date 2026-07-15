# OBS Live Delay Plugin

An OBS plugin for putting a delay on a stream after you have already gone live.

It switches to a holding scene while the delay is building or changing, then switches back when the delayed feed is ready. That gives a producer time to moderate without stopping the stream.

## Where it is now

This is early work, not a ready-to-install streaming plugin yet.

The packet buffer, delay state changes, dock, holding-scene switching, hotkeys, and OBS output wrapper are in place. The RTMP/FLV sender still needs to be built, so the plugin will not start a real stream yet.

## Building it

You need the OBS development build, Qt 6, CMake, and Microsoft C++ Build Tools. The current project uses CMake:

```powershell
cmake -S . -B build -DACTIVE_DELAY_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release
```

## Installing a future release

When releases are available, close OBS and extract the ZIP into its install folder. Open the plugin from **Docks → OBS Active Live Delay**.

The first version will start and stop the managed stream from the plugin dock, rather than OBS's normal **Start Streaming** button.
