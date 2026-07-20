# Active Live Delay TODO

This is the ordered engineering backlog. Detailed design and acceptance gates
are in [docs/MULTISTREAM-COMPATIBILITY-ROADMAP.md](docs/MULTISTREAM-COMPATIBILITY-ROADMAP.md).

Status legend: `[ ]` not started, `[~]` in progress or awaiting evidence,
`[x]` verified complete, `[-]` skipped/deferred by owner, `[!]` blocked by a
failed dependency/gate.

## P0 — establish the current baseline

Owner-directed status: complete by owner attestation on 2026-07-21. The local
OBS log confirms direct-output starts and stops; this status does not change the
beta label or constitute production acceptance without the recorded runtime
evidence.

- [x] Owner attests issue #5's non-critical Twitch acceptance is complete.
- [x] Owner attests delayed A/V sync, Close Delay, reconnect, longer session,
  stop, and OBS shutdown acceptance evidence is complete.
- [x] Owner attests baseline OBS logs and CPU/GPU/memory measurements are saved.
- [x] Keep v0.1.39 labelled beta until a separately approved release decision.

## P1 — architecture and characterization

- [x] Draft ADR-001 for native fan-out plus a Delayed Program Source.
- [x] Draft the multistream/source compatibility implementation roadmap.
- [x] Approve the measurable acceptance thresholds in the roadmap (2026-07-21).
- [x] Create gated implementation issues #6 through #17 from the roadmap breakdown.
- [x] Add characterization tests for current timestamp, discontinuity, packet
  release, and output-failure behaviour.

## P2 — reusable delayed-media pipeline

- [x] Add explicit Direct Single, Native Multistream, and Compatibility Source
  operating modes.
- [x] Refactor `ActiveDelaySession` into controller, codec, consumer, and
  lifecycle responsibilities.
- [x] Add `ReleasedPacketDispatcher` and immutable released batches.
- [x] Move current mux/send handling behind `NetworkPacketConsumer`.
- [x] Add epoch/discontinuity events and tests.
- [x] Prove byte-equivalent deterministic FLV output for the current path.
- [x] Add stable non-secret diagnostic codes for mode conflicts, network-consumer
  startup/dispatch, and discontinuity-notification failures.
- [x] Publish a per-code diagnostic error reference with safe operator guidance.

## P3 — native multistream (experimental)

- [x] Add destination model, validation, versioned storage, and secret redaction.
- [x] Refactor sender workers for independent target queues and status.
- [x] Add `MultiTargetSender` with concurrent non-UI-thread startup.
- [x] Implement primary/secondary failure policy.
- [x] Add per-target dock status and aggregate state.
- [x] Add fake-server failure, backpressure, reconnect, and shutdown tests.
- [~] Run recorded two-platform runtime acceptance (owner-required; no runtime claim from automated tests).

## P4 — Delayed Program Source feasibility (skipped / deferred)

Owner direction (2026-07-21): skip the Compatibility Source feasibility gate
for the Direct Single workflow. This is an owner scope decision, not a passed
feasibility gate. Do not begin P5 or represent Compatibility Source as
supported unless the owner explicitly resumes it and the P4 runtime gate
passes. Direct Single remains the primary supported workflow.

- [-] Isolated-scene capture prototype retained behind its opt-in build flag.
- [-] Scene-scoped audio, recursion, lifecycle, A/V, and resource runtime gate.
- [-] ADR feasibility decision; no Compatibility Source implementation begins.

## P5 — Delayed Program Source implementation (skipped / deferred)

- [ ] Add FFmpeg `avcodec`, `swscale`, and `swresample` dependencies.
- [ ] Implement and test H.264/AAC decoding, flush, reordering, scaling, and
  resampling.
- [ ] Register the asynchronous video/audio OBS source.
- [ ] Add holding-frame/silence priming and a READY gate.
- [ ] Add decoder/source health snapshots to the dock.
- [ ] Handle Return Live, Emergency Dump, target changes, and discontinuities.
- [ ] Verify removal, collection changes, repeated starts/stops, and shutdown.

## P6 — third-party compatibility

- [ ] Test and document the pinned Aitum version through Compatibility Source.
- [ ] Test and document the pinned SE.Live version through Compatibility Source.
- [ ] Ask Aitum for a stable public encoded-packet/destination integration API.
- [ ] Ask StreamElements for a supported integration API.
- [ ] Do not read private configuration or credentials from either plugin.
- [ ] Evaluate versioned adapters only after a supported API exists.

## P7 — fallback and later work

- [ ] If isolated source audio fails, prototype a loopback-only two-OBS bridge.
- [ ] Evaluate additional native renditions only after same-rendition fan-out is
  accepted.
- [ ] Resume dock UI backlog after streaming behaviour is stable.
- [ ] Do not claim production readiness until every release-specific acceptance
  criterion has matching runtime evidence.
