# OBS Live Delay Plugin

<p align="center">
  <img src="docs/production-warning.svg" width="100%" alt="Beta warning: v0.1.39 uses the direct-start workflow. Start and stop streaming from the plugin dock, not OBS's normal Start Streaming button. Test before production use.">
</p>

## Beta 5.1

Version `v0.1.39` is a licensing and packaging correction to Beta 5 of an OBS plugin for adding or changing stream delay after you are already live. It contains the same streaming implementation initially tested in v0.1.38. The initial user-run Twitch test reports that the new direct-start workflow works. Broader A/V, reconnect, delay-change, emergency-dump, and lifecycle acceptance testing is still required before production use.

Beta 5 replaces the unreliable live handoff with a plugin-owned stream from the beginning. The plugin creates H.264/AAC encoders from the active OBS **Simple Output** profile, connects to the configured streaming service, and keeps that connection while delay is added or removed. The old normal-OBS-output handoff is blocked because stopping the normal output caused Twitch to end the broadcast.

This release also converts OBS H.264 Annex-B packets to the length-prefixed AVC format required by FLV/RTMP, validates AVC packet structure, and waits for the first encoded frames before requiring H.264/AAC codec headers.

## Requirements

- OBS Studio 32.1.2 on Windows x64
- OBS **Settings → Output → Output Mode: Simple**
- An H.264 streaming encoder such as NVENC H.264, x264, QSV H.264, or AMD H.264
- AAC streaming audio
- Your Twitch or other RTMP streaming service configured normally in OBS

## Install

1. Close OBS completely.
2. Download `obs-active-live-delay-v0.1.39-windows-x64.zip` from the release.
3. Extract the ZIP into the OBS installation directory, normally:

   ```text
   C:\Program Files\obs-studio
   ```

4. Start OBS and open **Docks → Active Live Delay** if the dock is not visible.

## How to use it

Do not press OBS's normal **Start Streaming** button. The dock owns the stream connection in this beta.

### Start streaming and add delay

1. In the Active Live Delay dock, select a **Holding Scene**.
2. Press **Start Delayed Output**.
3. Wait until **Delayed Output** says `ACTIVE`, then confirm the platform shows you live.
4. Enter the desired **Target Delay (sec)**.
5. Press **Enable / Set Delay**.
6. The holding scene is shown while the buffer builds. For example, a 15-second delay needs roughly 15 seconds of buffering.
7. When the target is ready, the dock reports `DELAYED` and restores the original scene.

### Remove or reduce delay

- Press **Return Live** to clear the delay while keeping the broadcast running.
- Press **Emergency Dump** to reduce the current delay by the configured **Emergency Dump (sec)** value.
- Enter another target and press **Enable / Set Delay** to change the delay.

### End the broadcast

Press **Stop Delayed Output**. This ends the streaming connection completely.

## Button reference

| Button | Effect |
|:---|:---|
| **Start Delayed Output** | Starts the platform broadcast through the plugin. Press this instead of OBS **Start Streaming**. |
| **Stop Delayed Output** | Ends the platform broadcast completely. |
| **Enable / Set Delay** | Starts or changes the delay while the plugin output is active. |
| **Return Live** | Removes the delay but keeps streaming. |
| **Emergency Dump** | Reduces the current delay by the configured dump amount. |

## What is implemented

- Plugin-owned direct streaming output using the active Simple Output profile
- H.264/AAC codec-header startup after the first encoded frames
- Annex-B-to-AVC H.264 conversion before FLV muxing
- Set or change target delay from the OBS dock
- Return live and clear the delay buffer without ending the broadcast
- Holding-scene switching while delay builds
- Hotkeys for delay, return live, and emergency dump
- Keyframe-safe delayed playback
- Bounded media and sender queues
- FLV muxing on a normalized timeline
- RTMP/RTMPS sending with reconnect and keyframe realignment
- Persistent output errors and zero-frame detection
- Reopenable OBS dock and clean frontend-exit handling
- `en-US` and `en-GB` locale packaging

## Important limitations

- OBS's normal **Start Streaming** button and status bar do not own or represent the plugin output.
- Direct start currently requires OBS **Simple Output** mode with H.264 video and AAC audio.
- Do not try to switch an already-running normal OBS stream into the plugin; that workflow is blocked because Twitch ended the broadcast during handoff.
- This is still a beta. Test with a non-critical stream before relying on it.
- Return Live, Emergency Dump, reconnect behavior, long sessions, and clean shutdown still need broader independent runtime coverage.

## Build it

You need an OBS development build, Qt 6, CMake, and the Microsoft C++ Build Tools.

To run the dependency-free core tests without OBS or Qt:

```powershell
cmake -S . -B build-core -DACTIVE_DELAY_BUILD_PLUGIN=OFF -DACTIVE_DELAY_BUILD_TESTS=ON
cmake --build build-core --config Release
ctest --test-dir build-core -C Release --output-on-failure
```

Building the DLL also needs the OBS development libraries available through `CMAKE_PREFIX_PATH`. The full plugin build adds an OBS-linked packet-conversion test; the local Release suite currently contains five CTest targets.

## License

Copyright © 2026 Abhisek571 and contributors.

OBS Live Delay Plugin is free software licensed under the [GNU General Public License version 2 or later](LICENSE) (`GPL-2.0-or-later`). It is provided without warranty. The release ZIP includes both the complete license text and the project copyright notice.

OBS Studio, libobs, FFmpeg, Qt, and other third-party components retain their own copyright and license terms.

## Status

The direct-start OBS-to-Twitch path has an initial successful user report from the v0.1.38/v0.1.39 implementation, and all five automated test targets pass. Keep [issue #4](https://github.com/Abhisek571/OBS-Live-Delay-Plugin/issues/4) open until the delayed feed, A/V sync, Return Live, Emergency Dump, reconnect, and full lifecycle have recorded acceptance results.
