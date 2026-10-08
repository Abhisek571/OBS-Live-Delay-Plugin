# OBS Live Delay Plugin

<p align="center">
  <img src="docs/production-warning.svg" width="100%" alt="Alpha: v0.2.0-alpha1 — read the entire README before use. Not yet tested on live platforms. x264 only; NVIDIA NVENC is not supported yet.">
</p>

## v0.2.0-alpha1

OBS Live Delay Plugin adds a "panic button" delay to a live stream from an OBS
dock. You broadcast live, and the plugin always keeps the last few seconds in a
buffer. When you need a delay, **Start Delay** rewinds viewers into that buffer
at once; **Return Live** jumps them back to live. It can send the same stream to
up to three destinations.

It is alpha software: use a non-critical test stream before relying on it.

### New in v0.2.0-alpha1

- The broadcast now starts **live**, not delayed. The delay is always ready in
  the background instead of having to build up first.
- **Start Delay** is instant and shows no holding scene; **Return Live** keeps
  the buffer so you can delay again straight away.
- New **Emergency Dump (Rebuild Delay)** button.
- The dock shows **OFF AIR** whenever nothing is being broadcast.

See the [changelog](CHANGELOG.md) for the full list.

## Requirements

- OBS Studio 32.2.1 on Windows x64
- OBS **Settings → Output → Output Mode: Simple**
- The **x264** (software) H.264 streaming encoder. NVENC H.264 is currently
  refused at **Arm Buffer** with `TRANSITION_CODEC_UNSUPPORTED`, because its
  stream headers do not yet pass the safe-switch timing check. QSV and AMD
  H.264 are untested.
- AAC streaming audio
- One primary streaming service configured normally in OBS
- This alpha has been tested against local RTMP servers only. It has not yet
  been tested on Twitch, YouTube, Kick, or any other live platform.

## Install

1. Close OBS completely.
2. Download `obs-active-live-delay-v0.2.0-alpha1-windows-x64.zip` from the release.
3. Extract the ZIP into the OBS installation directory, normally:

   ```text
   C:\Program Files\obs-studio
   ```

4. Start OBS and open **Docks → Active Live Delay** if the dock is not visible.

## How to use it

Do not press OBS's normal **Start Streaming** button. The dock owns the stream connection in this alpha.

### Start a broadcast

The broadcast goes out live. The plugin keeps the last N seconds of programme
in a rolling buffer the whole time, so a delay can start instantly.

1. In the Active Live Delay dock, choose the **Delay length (seconds)** and the
   **Scene shown while an Emergency Dump rebuilds the delay** (the holding scene).
2. Press **Arm Buffer**. The plugin starts capturing your programme; the status
   shows `FILLING — OFF AIR`, then `READY — OFF AIR` once the buffer is full.
3. Press **Start Broadcast (Live)** at any point after arming. The platform
   receives your programme live. The status shows `BROADCASTING`, and the buffer
   keeps filling in the background.

The holding scene is rendered privately with its own encoder. It never changes
your OBS programme scene or recording.

### While broadcasting

- **Start Delay** rewinds the stream into the buffer at once and stays that many
  seconds behind. Viewers see the last N seconds again; no holding scene is
  shown. It is available once the buffer is full (N seconds after arming).
- **Return Live (Keep Broadcasting)** jumps forward to your current programme at
  the next keyframe. Viewers skip what was still delayed. The buffer is kept, so
  **Start Delay** is available again straight away. Connections stay up.
- **Emergency Dump (Rebuild Delay)** is available while delayed. It discards
  everything still buffered and rebuilds the same delay behind the holding
  scene. Content already sent to the platform cannot be recalled.

The delay length is locked once armed. To change it, use **Stop / Disarm**,
choose the new length, and arm again.

**Start Delay** and **Return Live** can also be bound to keys in OBS
**Settings → Hotkeys** (search for "Active Live Delay").

### Reading the dock status

| Delay state | Meaning |
|:---|:---|
| `OFF AIR` | Nothing is being broadcast (not armed, filling, ready, or stopped). |
| `LIVE (NO DELAY)` | Broadcasting live. The buffer is filling or full in the background. |
| `DELAYED` | Broadcasting the set number of seconds behind live. |
| `BUILDING DELAY` | Emergency Dump is rebuilding the delay; viewers see the holding scene. |
| `TRANSITION PENDING` | A switch is waiting for a safe point in the stream. It clears by itself. |

### End the broadcast

Press **Stop / Disarm** and confirm. This discards the buffer and ends the
primary and every enabled secondary broadcast.

### Experimental: Native Multistream (three destinations)

Native Multistream is experimental. The dock can send the same stream to up to
three destinations:

| Destination | Where you set it up |
|:---|:---|
| **Primary** | OBS **Settings → Stream**. Its card in the dock is read-only and shows status only. |
| **Secondary 1** | Its card in the dock (unlock, then open settings). |
| **Secondary 2** | Its card in the dock (unlock, then open settings). |

Each secondary can be turned on or off on its own. Each secondary card
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
| **Arm Buffer** | Starts capturing your programme into the rolling buffer. Nothing connects to a platform yet. |
| **Start Broadcast (Live)** | Available once armed. Connects the primary and every enabled secondary and broadcasts live while the buffer keeps filling. Press this instead of OBS **Start Streaming**. |
| **Stop / Disarm** | Discards the buffer and ends the primary and every enabled secondary. Requires confirmation while broadcasting. |
| **Start Delay** | Rewinds the stream into the full buffer at once and stays that many seconds behind. Viewers see the last seconds again. |
| **Return Live (Keep Broadcasting)** | Jumps forward to live and keeps the buffer, so Start Delay works again straight away. Connections stay online. |
| **Emergency Dump (Rebuild Delay)** | Available while delayed; requires confirmation. Discards buffered programme and rebuilds the same delay behind the holding scene. |

## Before you use it

- OBS's normal **Start Streaming** button and status bar do not own or represent the plugin output.
- Direct start currently requires OBS **Simple Output** mode with x264 H.264 video and AAC audio.
- The holding scene runs on its own x264 encoder for the whole session, so expect extra CPU use.
- Do not try to switch an already-running normal OBS stream into the plugin; that workflow is blocked because Twitch ended the broadcast during handoff.
- This is an alpha. Test with a non-critical stream before relying on it.
- This version has not been tested on any live platform yet. Do not infer
  support for Twitch or any other RTMP service.
- Return Live, reconnect behaviour, long sessions, and clean shutdown need broader testing.
- Native Multistream is experimental. It has been tested with three local RTMP
  servers only; Twitch, YouTube, and Kick together are untested.
- If the dock reports an `ALD-E####` error during testing, record the code and
  safe message with the OBS log. See the [error-code guide](docs/ERROR-CODES.md).

## For contributors

- [Build and test instructions](docs/BUILDING.md)
- [Technical design and current development notes](docs/TECHNICAL-NOTES.md)
- [Project wiki](https://github.com/Abhisek571/OBS-Live-Delay-Plugin/wiki)

## License

Copyright © 2026 Abhisek571 and contributors.

OBS Live Delay Plugin is free software licensed under the [GNU General Public License version 2 or later](LICENSE) (`GPL-2.0-or-later`). It is provided without warranty. The release ZIP includes both the complete license text and the project copyright notice.

OBS Studio, libobs, FFmpeg, Qt, and other third-party components retain their own copyright and license terms.
