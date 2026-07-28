# Changelog

Release labels use `v0.1.NN-betaNN` for beta `NN`; for example, Beta 5 uses
`v0.1.5-beta5` and Beta 5.2 uses `v0.1.52-beta52`.

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
