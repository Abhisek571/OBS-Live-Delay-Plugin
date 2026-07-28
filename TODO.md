# Active Live Delay TODO

This is the ordered engineering backlog. Detailed design and acceptance gates
are in [docs/MULTISTREAM-COMPATIBILITY-ROADMAP.md](docs/MULTISTREAM-COMPATIBILITY-ROADMAP.md).

Status legend: `[ ]` not started, `[~]` in progress or awaiting evidence,
`[x]` verified complete, `[-]` skipped/deferred by owner, `[!]` blocked by a
failed dependency/gate.

## Owner testing direction

Owner direction (2026-07-28): do not request or schedule owner-operated manual
or runtime tests during implementation phases. Continue automated verification
throughout implementation. Defer every pending manual check to one combined
final acceptance gate after all authorized implementation work and automated
checks are complete. Deferred manual items do not block implementation, but
they remain required before platform-support, production-readiness, or release
acceptance claims.

## P0 — establish the current baseline

Owner-directed status: complete by owner attestation on 2026-07-21. The local
OBS log confirms direct-output starts and stops; this status does not change the
beta label or constitute production acceptance without the recorded runtime
evidence.

- [x] Owner attests issue #5's non-critical Twitch acceptance is complete.
- [x] Owner attests delayed A/V sync, Return Live, reconnect, longer session,
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
- [x] Refactor `ActiveDelaySession` into controller, consumer, multistream, and
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
- [-] Two-platform runtime acceptance deferred into the combined final owner
  acceptance gate; no runtime claim may be made from automated tests.

### P3.1 — three-platform dock and Kick support

Owner direction (2026-07-21): make Native Multistream easy to configure for
three simultaneous platforms. The bounded scope is one read-only primary OBS
service plus two plugin-owned secondary destinations. This reuses one delayed
H.264/AAC rendition; it does not add per-platform encoders or restore the unsafe
normal-output handoff.

Research notes:

- Kick requires the user to copy both **Server URL** and **Stream Key** from
  Creator Dashboard -> Channel -> Stream URL and Key. Do not guess, scrape, or
  hard-code a Kick publish URL.
- Kick's current documented common-rendition limits are H.264, CBR, a 2-second
  keyframe interval, at most 1920x1080 at 60 fps, and at most 8,000 kbps.
- YouTube recommends RTMPS and tells users to copy its current Stream URL and
  key from Live Control Room. Twitch likewise assigns the key in Creator
  Dashboard and uses a selected ingest server. Platform presets must therefore
  provide labels and safe setup guidance, not silently invent credentials or
  assume an endpoint will remain stable.
- Official references: [Kick setup](https://help.kick.com/en/articles/7066931-how-to-stream-on-kick-com),
  [YouTube RTMPS](https://support.google.com/youtube/answer/10364924), and
  [Twitch stream key/ingest format](https://help.twitch.tv/s/article/twitch-stream-key-faq).

- [x] Replace the single-secondary form with three compact destination cards:
  **1 Primary (OBS Settings)**, **2 Secondary**, and **3 Secondary**. Keep the
  primary card read-only and allow each secondary card to be enabled separately.
- [x] Give secondary cards a platform selector for **Custom RTMP**, **Twitch**,
  **YouTube**, and **Kick**; show platform-specific instructions and placeholders
  while still requiring the user to paste the official current server URL and
  stream key.
- [x] Mask every stream key by default, add an intentional press-and-hold reveal
  action, and never put keys or complete publish URLs in status text, logs, or
  copied diagnostics.
- [x] Show one clear row per destination with platform/name, connection state,
  reconnect count, queue/error state, and sent bitrate or bytes where available.
  Colour may reinforce state but must not replace text.
- [x] Add a preflight summary before start: enabled platform count, estimated
  total upload bitrate, and warnings when the shared OBS rendition exceeds a
  selected platform's documented codec, resolution, frame-rate, bitrate, or
  keyframe limits. Fail only on known hard incompatibilities; keep guidance
  distinct from runtime acceptance.
- [x] Keep destination editing disabled while output is active. Make **Return
  Live** clearly preserve all platform connections and require confirmation for
  **End Broadcast**, which ends the primary and every enabled secondary broadcast.
- [x] Bump destination storage to version 2 and migrate the existing version-1
  secondary into destination 2 without losing its locally stored secret.
- [x] Extend validation and fake-server tests to primary plus two secondaries:
  identical FLV order/timestamps, independent queues, one-secondary failure,
  two-secondary shutdown, duplicate identity rejection, migration, and complete
  secret redaction.
- [x] Add `en-US` and `en-GB` strings plus narrow-dock UI checks for all new
  destination controls, warnings, status text, and destructive confirmations.
- [-] Three-platform manual acceptance deferred into the combined final owner
  acceptance gate in P5. Do not claim Kick support before that evidence is
  saved.

## P4 — Compatibility Source and Aitum (deferred)

Owner direction (2026-07-21): focus on the plugin's own Native Multistream path.
Aitum, SE.Live, Delayed Program Source, and the two-OBS bridge are not required
for the three-platform goal. Keep the existing design and opt-in probe for future
reference, but do not spend implementation or runtime-acceptance effort here
unless the owner explicitly resumes this scope.

- [-] Delayed Program Source isolated-scene video/audio feasibility gate.
- [-] FFmpeg decoder and asynchronous OBS source implementation.
- [-] Aitum and SE.Live compatibility acceptance.
- [-] Supported third-party adapter discussions and versioned integrations.
- [-] Loopback two-OBS fallback.
- [x] Keep Native Multistream independent of all third-party plugins and never
  read their configuration or credentials.

## P5 — native multistream release gate and later work

- [x] Complete the P3.1 three-destination storage, dock UI, validation, status,
  and fake-server coverage.
- [x] Run the dependency-free core suite and full OBS-linked suite after every
  Native Multistream change.
- [~] `git diff --check` passes. The code-review graph tool was unavailable, so
  source/`rg` mapping and the full automated suites were used as the documented
  fallback; graph refresh remains pending until the tool is available.
- [~] After every authorized implementation phase and automated check is
  complete, ask the owner to run one combined final manual acceptance. It must
  cover Direct Single plus Twitch, YouTube, and Kick Native Multistream;
  delayed A/V and <= 250 ms inter-destination presentation difference; one
  secondary failure/reconnect; Return Live; a long session; End Broadcast;
  and clean OBS shutdown.
- [x] Do not request partial smoke tests or platform checks before that final
  gate. Keep all manual acceptance items pending until the owner records the
  combined result.
- [ ] Keep the release beta until the required three-platform evidence passes.
- [-] Do not advertise Aitum or Compatibility Source support in this release.
