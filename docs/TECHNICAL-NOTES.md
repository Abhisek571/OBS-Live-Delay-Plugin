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

- **Arm** starts a `PipelineOwner` that captures compressed programme off air
  into the `DelayController`. No destination is read or connected until the
  buffer is `READY` and the user presses **Start Delayed Broadcast**.
- The holding scene is rendered by an isolated `obs_view` with its own x264
  encoder and generated silent AAC (`holding-obs-capture.cpp`). It runs for the
  whole session and never changes the OBS programme or recording.
- `TransitionCoordinator` splices holding and programme inside the outgoing
  stream. Each action reserves a new epoch, fences queued network media, sends
  a silent AAC drain, paces holding, and resumes programme on a fresh keyframe
  with a silent audio bridge. Pacing stops once the resume boundary is
  delivered.
- `transition-codec.hpp` admits only codecs whose switch timing it can prove:
  progressive AVC with VUI timing and bounded reordering, and 1024-sample LC
  AAC at 44.1/48 kHz. NVENC headers currently fail this check.
- H.264 Annex-B packets are converted to FLV-compatible length-prefixed AVC.
- Sender reconnects realign on a video keyframe. A reconnect caused by a
  transition replaces the cancelled transport and keeps the new epoch's media.

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

## Main code areas

- `src/active-delay-dock.cpp/.hpp`: dock UI, direct encoders/service creation, controls, scenes, lifecycle
- `src/active-delay-output.cpp/.hpp`: output callbacks, startup, packet flow, errors, sender lifecycle
- `src/delay-controller.cpp/.hpp`: delay state machine and buffered release
- `src/flv-muxer.cpp/.hpp`: FLV tags, AVC validation, timing
- `src/rtmp-sender.cpp/.hpp`: bounded asynchronous send/reconnect logic
