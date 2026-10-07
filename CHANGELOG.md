# Changelog

Release labels use `v0.1.NN-betaNN` for beta `NN`; for example, Beta 5 uses
`v0.1.5-beta5`, Beta 5.2 uses `v0.1.52-beta52`, and Beta 5.3 uses
`v0.1.53-beta53`.

## Unreleased

- Replaces the live-then-delay flow with **Arm Buffer → READY → Start Delayed
  Broadcast**: the delay fills off air, and the platform receives programme
  already delayed.
- Renders the holding scene privately with its own encoder and switches to it
  inside the outgoing stream, without touching the OBS programme or recording.
- Adds **Emergency Dump (Rebuild Delay)**.
- Fixes: Start Delay while delayed no longer leaves a hole in the buffer;
  switching to holding no longer leaves an audio gap; pacing stops once a resume
  is on air, removing lasting latency and a spurious `TRANSITION_GUARD_OVERFLOW`;
  a transition's own reconnect keeps its drain guard instead of stalling.
- Fixes multistream delivery: reconnecting secondaries are no longer shut off,
  a secondary stalled mid-transition is isolated instead of ending the broadcast,
  and a transition during a reconnect or drain write no longer fails the
  destination.
- Hardening: holding outputs register at plugin load; concurrent audio-output
  stops are serialized.
- NVENC H.264 is refused at Arm (`TRANSITION_CODEC_UNSUPPORTED`); use x264.
- Automated core and OBS-linked checks pass. Platform runtime acceptance is
  still pending; this is not a production claim.

## v0.1.53-beta53

- Adds the completed experimental three-destination Native Multistream dock:
  the primary OBS destination plus two independently enabled secondary RTMP or
  RTMPS destinations.
- Adds Custom RTMP, Twitch, YouTube, and Kick labels, masked stream-key entry,
  preflight validation, per-destination status, and isolated sender queues.
- Adds scene-switch lifecycle safeguards, clearer broadcast/delay controls,
  stable diagnostics, and OBS 32.2.1 build compatibility.
- Automated core and OBS-linked checks pass. Twitch, YouTube, and Kick
  combined runtime acceptance remains pending; this is not a production claim.

## v0.1.52-beta52

- Corrects the prerelease version label for the Native Multistream beta.
- States clearly that runtime acceptance currently covers Twitch only; other
  platforms and RTMP services are untested.
- Streaming binary is unchanged from v0.1.40 — Beta 5.2.

## v0.1.40 — Beta 5.2

- Marks Native Multistream as **experimental** in the dock and documentation.
- Keeps Native Multistream limited to test/non-critical destinations until its
  recorded two-platform runtime acceptance is complete.
- Includes the completed packet-dispatch, independent-target queue, target
  status, configuration validation, redaction, and diagnostic-code work from
  the Native Multistream implementation phase.

## v0.1.39 — Beta 5.1

- Licensing and packaging correction to v0.1.38; streaming DLL unchanged.
