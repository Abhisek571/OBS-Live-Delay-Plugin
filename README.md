# OBS Live Delay Plugin

<p align="center">
  <img src="docs/production-warning.svg" width="100%" alt="Beta: v0.1.53-beta53 — read the entire README before use. Twitch tested only; other platforms untested.">
</p>

## v0.1.53-beta53

Active Live Delay lets you add, change, remove, or reduce a stream delay from
an OBS dock. It is still beta software: use a non-critical test stream before
relying on it.

## Requirements

- OBS Studio 32.2.1 on Windows x64
- OBS **Settings → Output → Output Mode: Simple**
- The **x264** (software) H.264 streaming encoder. NVENC H.264 is currently
  refused at **Arm Buffer** with `TRANSITION_CODEC_UNSUPPORTED`, because its
  stream headers do not yet pass the safe-switch timing check. QSV and AMD
  H.264 are untested.
- AAC streaming audio
- One primary streaming service configured normally in OBS
- This version has runtime acceptance on Twitch only; other platforms and RTMP
  services are untested.

## Install

1. Close OBS completely.
2. Download `obs-active-live-delay-v0.1.53-beta53-windows-x64.zip` from the release.
3. Extract the ZIP into the OBS installation directory, normally:

   ```text
   C:\Program Files\obs-studio
   ```

4. Start OBS and open **Docks → Active Live Delay** if the dock is not visible.

## How to use it

Do not press OBS's normal **Start Streaming** button. The dock owns the stream connection in this beta.

### Start a delayed broadcast

The delay is built off air first, so viewers never see the buffer filling.

1. In the Active Live Delay dock, choose the **Scene shown while delay starts**
   (the holding scene) and the **Delay length (seconds)**.
2. Press **Arm Buffer**. The plugin captures your programme off air; the status
   shows `FILLING — OFF AIR`.
3. Wait for `READY — OFF AIR`. A 15-second delay needs about 15 seconds.
4. Press **Start Delayed Broadcast**. The platform receives your programme
   already delayed. The status shows `BROADCASTING`.

The holding scene is rendered privately with its own encoder. It never changes
your OBS programme scene or recording.

### While broadcasting

- **Return Live (Keep Broadcasting)** drops the delay and resumes your current
  programme at the next keyframe. Connections stay up.
- **Start Delay** rebuilds the armed delay behind the holding scene, for example
  after Return Live.
- **Emergency Dump (Rebuild Delay)** discards everything still buffered and
  rebuilds the same delay behind the holding scene. Content already sent to the
  platform cannot be recalled.

The delay length is locked once armed. To change it, use **Stop / Disarm**,
choose the new length, and arm again.

### End the broadcast

Press **Stop / Disarm** and confirm. This discards the buffer and ends the
primary and every enabled secondary broadcast.

### Experimental: Native Multistream (three destinations)

Native Multistream is experimental. The dock can send the same delayed
H.264/AAC rendition to the read-only primary OBS streaming service plus two
independently enabled secondary RTMP/RTMPS destinations. Each secondary card
offers Custom RTMP, Twitch, YouTube, and Kick labels. The label supplies setup
guidance only: paste the current official server URL and stream key yourself.
The plugin does not guess endpoints or read credentials from another plugin.

Stream keys stay masked unless **Hold to reveal** is pressed and held. Version-2
profile storage migrates the previous single secondary into destination 2 and
preserves its locally stored key. Keys and complete publish URLs are excluded
from target status and plugin diagnostics.

Before start, **Preflight** shows the enabled destination count, estimated total
upload bitrate, and known shared-rendition issues. Known hard incompatibilities
block start with `ALD-E2016`; guidance does not claim runtime compatibility.
Kick currently requires H.264, CBR, two-second keyframes, at most 1920x1080 at
60 fps, and at most 8,000 kbps. Always check the platform's current official
requirements.

The primary OBS service owns the output state. A failed or slow secondary is
shown as `FAILED` on its destination row but must not stop the primary or the
other secondary. Rows show connection state, reconnect count, queue use, error
code, and sent bytes. Destination editing is disabled while output is active.
Twitch, YouTube, and Kick combined runtime acceptance remains pending.

## Button reference

| Button | Effect |
|:---|:---|
| **Arm Buffer** | Captures your programme off air and fills the delay. Nothing connects to a platform yet. |
| **Start Delayed Broadcast** | Available at `READY`. Connects the primary and every enabled secondary and starts already delayed. Press this instead of OBS **Start Streaming**. |
| **Stop / Disarm** | Discards the buffer and ends the primary and every enabled secondary. Requires confirmation while broadcasting. |
| **Start Delay** | Rebuilds the armed delay behind the holding scene. |
| **Return Live (Keep Broadcasting)** | Removes the delay but keeps all platform connections online. |
| **Emergency Dump (Rebuild Delay)** | Requires confirmation. Discards buffered programme and rebuilds the same delay behind the holding scene. |

## Before you use it

- OBS's normal **Start Streaming** button and status bar do not own or represent the plugin output.
- Direct start currently requires OBS **Simple Output** mode with x264 H.264 video and AAC audio.
- The holding scene runs on its own x264 encoder for the whole session, so expect extra CPU use.
- Do not try to switch an already-running normal OBS stream into the plugin; that workflow is blocked because Twitch ended the broadcast during handoff.
- This is still a beta. Test with a non-critical stream before relying on it.
- The current version has runtime acceptance on Twitch only. Do not infer
  support for any other platform or RTMP service.
- Return Live, reconnect behaviour, long sessions, and clean shutdown need broader testing.
- Native Multistream is experimental and has automated three-destination
  fake-server coverage only. Recorded Twitch, YouTube, and Kick runtime
  acceptance remains required.
- If the dock reports an `ALD-E####` error during testing, record the code and
  safe message with the OBS log. See the [error-code guide](docs/ERROR-CODES.md).

## For contributors

- [Build and test instructions](docs/BUILDING.md)
- [Technical design and current development notes](docs/TECHNICAL-NOTES.md)
- [Final combined multistream acceptance](docs/FINAL-MULTISTREAM-ACCEPTANCE.md)
- [Project wiki](https://github.com/Abhisek571/OBS-Live-Delay-Plugin/wiki)

## License

Copyright © 2026 Abhisek571 and contributors.

OBS Live Delay Plugin is free software licensed under the [GNU General Public License version 2 or later](LICENSE) (`GPL-2.0-or-later`). It is provided without warranty. The release ZIP includes both the complete license text and the project copyright notice.

OBS Studio, libobs, FFmpeg, Qt, and other third-party components retain their own copyright and license terms.
