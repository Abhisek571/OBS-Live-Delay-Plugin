# Active Live Delay — agent guidance

## Scope and safety

- This is a Windows x64 OBS Studio 32.2.1 plugin. Preserve the working direct
  single-output path while adding compatibility work behind explicit modes.
- Never restore the normal-OBS-output handoff. Stopping that output can end a
  live platform broadcast.
- Never log, commit, expose, or scrape stream keys, passwords, or third-party
  plugin credentials.
- Do not describe a build or unit-test result as production acceptance. Runtime
  delayed A/V, reconnect, long-session, Return Live, Emergency Dump, stop, and
  OBS-shutdown evidence are required.
- Preserve unrelated worktree changes. Do not delete releases/tags or close
  runtime-acceptance issues without explicit user approval and evidence.
- Before the owner performs the final manual Twitch acceptance test, add stable
  non-secret diagnostic error codes for new operational failure paths. Do not
  run or claim the full manual Twitch acceptance workflow on the owner's
  behalf.
- Owner direction on 2026-07-28 defers all owner-operated manual and runtime
  testing until every authorized implementation phase and its automated checks
  are complete. Do not request phase-by-phase manual testing. Keep manual items
  pending for one combined final acceptance gate, and do not claim platform
  support, production readiness, or release acceptance before that evidence.
- Continue automated unit, component, integration, build, and static checks
  during each implementation phase. The manual-testing deferral does not waive
  automated verification or final runtime evidence.

## Communication

- Use concise caveman-style progress and handoff messages when speaking with
  the owner (for example, "me add error code; test later"). Keep code,
  documentation, and user-visible plugin errors clear and professional.

## Release versioning

- Beta releases use `v0.1.NN-betaNN`, where `NN` is the beta label with its
  decimal separator removed. For example, Beta 5 is `v0.1.5-beta5` and Beta
  5.2 is `v0.1.52-beta52`.
- Keep the CMake numeric project version, README, warning banner, changelog,
  Git tag, GitHub prerelease title, and Windows ZIP filename aligned with that
  release version.
- Preserve earlier tags and releases when correcting a version label unless
  the owner explicitly directs their deletion.

## Current architecture

The current direct path is:

`OBS H.264/AAC encoders -> packet conversion -> DelayController -> FLV muxer -> bounded sender queue -> RTMP/RTMPS`

Use released encoded packets as the extension boundary. Long delays must remain
compressed: do not introduce long raw-video frame buffers.

## Compatibility roadmap

- Keep operating modes explicit and initially mutually exclusive: Direct
  Single, Native Multistream, and Compatibility Source.
- Native multistream must give each target an independent bounded queue and
  must not let a failed secondary stall capture or the primary.
- A Delayed Program Source requires a feasibility spike proving isolated scene
  video and scene-scoped audio, recursion prevention, clean lifecycle, and
  acceptable A/V sync before source-feature implementation starts.
- If isolated scene audio cannot be implemented with supported libobs APIs,
  evaluate the loopback two-OBS bridge; do not silently substitute global audio.

## Build and test

For dependency-free core work:

```powershell
& "C:\Qt\Tools\CMake_64\bin\cmake.exe" -S . -B build-agent-core -DACTIVE_DELAY_BUILD_PLUGIN=OFF -DACTIVE_DELAY_BUILD_TESTS=ON
& "C:\Qt\Tools\CMake_64\bin\cmake.exe" --build build-agent-core --config Release
& "C:\Qt\Tools\CMake_64\bin\ctest.exe" --test-dir build-agent-core -C Release --output-on-failure
```

The full plugin build needs the local OBS SDK configuration from the primary
checkout. Prefer forward-slash CMake paths; do not copy source changes back
from a build directory. Run `git diff --check` before committing.

## Key code areas

- `src/delay-controller.*`: timing, buffering, state transitions.
- `src/released-packet-dispatcher.*`: immutable released-packet batches and
  consumer lifecycle.
- `src/flv-muxer.*` and `src/rtmp-sender.*`: direct/network delivery.
- `src/active-delay-output.*`: OBS output callbacks and live pipeline wiring.
- `src/active-delay-dock.*`: dock UI and OBS/frontend lifecycle.
- `tests/`: core and OBS-linked regression tests.
