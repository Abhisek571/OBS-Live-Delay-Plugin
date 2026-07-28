# Technical notes

This document is for contributors. It describes the current beta architecture,
not the end-user workflow.

## Streaming model

The dock owns a plugin-created direct output. It reads the active OBS Simple
Output profile, creates matching H.264/AAC encoders, applies the configured
streaming service, and opens the platform connection itself.

The normal OBS **Start Streaming** button does not own this output and must not
be used for the current beta. A previous normal-OBS-to-plugin handoff stopped
the normal OBS output after buffering; Twitch treated that interruption as the
end of the broadcast, so this design is blocked.

Media path:

`OBS encoders -> packet conversion -> DelayController -> FLV muxer -> bounded sender queue -> RTMP/RTMPS`

## Startup and playback behaviour

- Encoded capture begins before codec headers are required.
- Audio received before both H.264 and AAC headers is discarded.
- H.264 Annex-B packets are converted to FLV-compatible length-prefixed AVC.
- Delayed playback resumes on a video keyframe.
- The holding scene remains active while a delay builds or changes.
- Return Live clears buffered delay without intentionally ending the stream.
- Sender reconnects realign on a video keyframe.

## Current support boundary

- OBS Studio 32.2.1 on Windows x64
- Simple Output mode
- H.264 video and AAC audio
- Plugin-owned direct RTMP/RTMPS output

Enhanced Broadcasting, advanced multitrack output, and normal-output handoff
are not supported by this beta. Runtime acceptance remains required for A/V
sync, reconnect, long-session stability, Return Live, stopping,
and OBS shutdown.

## Planned compatibility architecture

Native Multistream fan-out is implemented as an experimental three-destination
mode: one primary OBS service plus two independently enabled secondaries. The
version-2 profile format, preflight, per-target metrics, and fake-server/UI
checks are automated evidence only. Recorded Twitch, YouTube, and Kick runtime
acceptance remains required before a platform-support or production claim.
A compressed Delayed Program Source remains proposed. The source path is
intended to let normal OBS, Aitum, SE.Live, and other output owners consume
delayed programme video/audio as an ordinary OBS source. It is gated on proving
isolated scene video and audio capture without a recursive scene path.

See `docs/MULTISTREAM-COMPATIBILITY-ROADMAP.md` and
`docs/architecture/ADR-001-multistream-compatibility.md`. The ordered checklist
is `TODO.md`.

## Main code areas

- `src/active-delay-dock.cpp/.hpp`: dock UI, direct encoders/service creation, controls, scenes, lifecycle
- `src/active-delay-output.cpp/.hpp`: output callbacks, startup, packet flow, errors, sender lifecycle
- `src/delay-controller.cpp/.hpp`: delay state machine and buffered release
- `src/flv-muxer.cpp/.hpp`: FLV tags, AVC validation, timing
- `src/rtmp-sender.cpp/.hpp`: bounded asynchronous send/reconnect logic
