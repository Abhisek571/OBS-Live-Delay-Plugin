# OBS Live Delay Plugin

<p align="center">
  <img src="docs/production-warning.svg" width="100%" alt="Beta: v0.1.52-beta52 — read the entire README before use. Twitch tested only; other platforms untested.">
</p>

## v0.1.52-beta52

Active Live Delay lets you add, change, remove, or reduce a stream delay from
an OBS dock. It is still beta software: use a non-critical test stream before
relying on it.

## Requirements

- OBS Studio 32.1.2 on Windows x64
- OBS **Settings → Output → Output Mode: Simple**
- An H.264 streaming encoder such as NVENC H.264, x264, QSV H.264, or AMD H.264
- AAC streaming audio
- A Twitch streaming service configured normally in OBS
- This version has runtime acceptance on Twitch only; other platforms and RTMP
  services are untested.

## Install

1. Close OBS completely.
2. Download `obs-active-live-delay-v0.1.52-beta52-windows-x64.zip` from the release.
3. Extract the ZIP into the OBS installation directory, normally:

   ```text
   C:\Program Files\obs-studio
   ```

4. Start OBS and open **Docks → Active Live Delay** if the dock is not visible.

## How to use it

Do not press OBS's normal **Start Streaming** button. The dock owns the stream connection in this beta.

### Start streaming and add delay

1. In the Active Live Delay dock, select a **Holding Scene**.
2. Press **Start Stream**.
3. Wait until **Delayed Output** says `ACTIVE`, then confirm the platform shows you live.
4. Enter the desired **Target Delay (sec)**.
5. Press **Enable Delay**.
6. The holding scene is shown while the buffer builds. For example, a 15-second delay needs roughly 15 seconds of buffering.
7. When the target is ready, the dock reports `DELAYED` and restores the original scene.

### Remove delay

- Press **Close Delay** to clear the delay while keeping the broadcast running.

### End the broadcast

Press **Stop Stream**. This ends the streaming connection completely.

### Experimental: Native Multistream (two RTMP destinations)

Native Multistream is experimental. The dock can send the same delayed
H.264/AAC rendition to the OBS streaming service plus one manual secondary
RTMP/RTMPS destination. Before starting, enable **Experimental Native
Multistream secondary** and enter its name, RTMP server, and stream key. These
settings are saved in the active OBS profile and are hidden from dock status and
plugin logs.

The primary OBS service owns the output state. A failed or slow secondary is
shown as `FAILED` in **Targets** but must not stop the primary. Destination
editing is disabled while the output is active. Test two non-critical platform
destinations, reconnect, Return Live, Emergency Dump, Stop Stream, and OBS
shutdown before relying on this beta workflow.

## Button reference

| Button | Effect |
|:---|:---|
| **Start Stream** | Starts the platform broadcast through the plugin. Press this instead of OBS **Start Streaming**. |
| **Stop Stream** | Ends the platform broadcast completely. |
| **Enable Delay** | Starts the configured delay while the plugin output is active. |
| **Close Delay** | Removes the delay but keeps streaming. |

## Before you use it

- OBS's normal **Start Streaming** button and status bar do not own or represent the plugin output.
- Direct start currently requires OBS **Simple Output** mode with H.264 video and AAC audio.
- Do not try to switch an already-running normal OBS stream into the plugin; that workflow is blocked because Twitch ended the broadcast during handoff.
- This is still a beta. Test with a non-critical stream before relying on it.
- The current version has runtime acceptance on Twitch only. Do not infer
  support for any other platform or RTMP service.
- Close Delay, reconnect behaviour, long sessions, and clean shutdown need broader testing.
- Native Multistream is experimental and has automated fake-server coverage
  only; recorded two-platform runtime acceptance remains required.
- If the dock reports an `ALD-E####` error during testing, record the code and
  safe message with the OBS log. See the [error-code guide](docs/ERROR-CODES.md).

## For contributors

- [Build and test instructions](docs/BUILDING.md)
- [Technical design and current development notes](docs/TECHNICAL-NOTES.md)

## License

Copyright © 2026 Abhisek571 and contributors.

OBS Live Delay Plugin is free software licensed under the [GNU General Public License version 2 or later](LICENSE) (`GPL-2.0-or-later`). It is provided without warranty. The release ZIP includes both the complete license text and the project copyright notice.

OBS Studio, libobs, FFmpeg, Qt, and other third-party components retain their own copyright and license terms.
