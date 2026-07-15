# OBS Live Delay Plugin

<p align="center">
  <img src="docs/production-warning.svg" width="100%" alt="Beta warning: v0.1.39 uses the direct-start workflow. Start and stop streaming from the plugin dock, not OBS's normal Start Streaming button. Test before production use.">
</p>

## Beta 5.1

Active Live Delay lets you add, change, remove, or reduce a stream delay from
an OBS dock. It is still beta software: use a non-critical test stream before
relying on it.

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

## Before you use it

- OBS's normal **Start Streaming** button and status bar do not own or represent the plugin output.
- Direct start currently requires OBS **Simple Output** mode with H.264 video and AAC audio.
- Do not try to switch an already-running normal OBS stream into the plugin; that workflow is blocked because Twitch ended the broadcast during handoff.
- This is still a beta. Test with a non-critical stream before relying on it.
- Return Live, Emergency Dump, reconnect behaviour, long sessions, and clean shutdown need broader testing.

## For contributors

- [Build and test instructions](docs/BUILDING.md)
- [Technical design and current development notes](docs/TECHNICAL-NOTES.md)

## License

Copyright © 2026 Abhisek571 and contributors.

OBS Live Delay Plugin is free software licensed under the [GNU General Public License version 2 or later](LICENSE) (`GPL-2.0-or-later`). It is provided without warranty. The release ZIP includes both the complete license text and the project copyright notice.

OBS Studio, libobs, FFmpeg, Qt, and other third-party components retain their own copyright and license terms.
