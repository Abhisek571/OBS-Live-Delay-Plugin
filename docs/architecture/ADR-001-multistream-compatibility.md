# ADR-001: Native fan-out plus a delayed-program compatibility source

## Status

Accepted as a gated future architecture. Owner direction on 2026-07-21 defers
Compatibility Source, Aitum, SE.Live, and the feasibility probe while the
plugin-owned Native Multistream path is completed. This is not a feasibility
pass and does not authorize source-feature implementation. If the owner resumes
this scope, owner direction on 2026-07-28 still defers manual/runtime testing
until the combined final acceptance gate.

## Context

Active Live Delay currently creates one H.264 encoder, one AAC encoder, one OBS
service, one custom output, one FLV muxer, and one RTMP sender. This direct-start
design fixed the unsafe normal-output handoff, but it bypasses output workflows
owned by Aitum Multistream, SE.Live, and similar plugins.

The compatibility requirement is broader than opening several sockets:

- every destination must observe the same delayed programme timeline;
- Return Live and discontinuities must remain
  synchronized;
- a slow or failed destination must not stall healthy destinations;
- Aitum and SE.Live should be able to own their normal OBS outputs when possible;
- stream keys must remain secret; and
- the existing single-output beta must remain usable while new work is staged.

The project is a real-time Windows OBS plugin maintained as a small modular C++
codebase. Simplicity, failure isolation, and evidence from non-critical runtime
tests take priority over a broad but fragile integration surface.

## Options considered

| Option | Advantages | Costs and risks | Complexity | Valid use |
|:---|:---|:---|:---|:---|
| Keep one output and recommend a cloud relay | No code change; one encode | External dependency; cost; no per-target status; not direct compatibility | Low | Temporary workaround |
| Restore normal-output handoff | Reuses OBS/Aitum output configuration | Already proved unsafe: stopping the original output can end a platform broadcast | Medium | Rejected |
| Native multi-target fan-out | One encode and one mux pass; efficient; controlled failure isolation | Does not make Aitum/SE.Live own the outputs | Medium | Preferred dependable multistream path |
| Delayed Program Source | Looks like an ordinary A/V source to OBS and output plugins; generic compatibility | Isolated scene/audio capture is uncertain; decode and re-encode cost; recursion risk | High | Preferred compatibility experiment |
| Per-source raw delay filter | Simple for one camera or media source | Does not delay the whole programme reliably; raw buffers are large; transitions and mixed audio are incomplete | Medium | Optional specialist feature |
| Local bridge into a second OBS instance | Strong isolation; works with normal output plugins | Two OBS instances; local transport; decode/re-encode; more setup | Medium | Fallback if in-process source capture fails |
| Private Aitum/SE.Live configuration scraping | Could copy configured targets | Fragile, insecure, undocumented, and vendor-version dependent | High | Rejected |
| Vendor-specific packet API adapters | Potentially efficient and seamless | Requires a stable public API and cooperation from each vendor | High/external | Later enhancement |

## Decision

Adopt a dual-path architecture:

1. Build native multi-target fan-out as the controlled, efficient way to send
   one delayed encoded programme to several RTMP/RTMPS destinations.
2. Prototype a compressed Delayed Program Source as a compatibility bridge for
   normal OBS, Aitum, SE.Live, recording, and other output owners.
3. Keep direct single-output mode until each replacement path independently
   passes its acceptance gates.
4. Do not resume the normal-output handoff and do not depend on private plugin
   configuration formats.

The common boundary will be released encoded packets, not raw frames and not
vendor-specific outputs. A dispatcher will take each batch from the
`DelayController` once and route it to the active consumer:

```text
OBS capture and H.264/AAC encoding
                 |
                 v
          DelayController
                 |
                 v
      ReleasedPacketDispatcher
          |                 |
          v                 v
  FLV/network branch   Decoder/source branch
          |                 |
          v                 v
   N target workers   OBS async A/V source
```

The first implementation may make the network and source modes mutually
exclusive. The dispatcher boundary must not prevent simultaneous consumers in
the future, but simultaneous operation is not an MVP requirement.

The supported product priority is Direct Single. Compatibility Source work is
not release scope unless the owner explicitly resumes it after the feasibility
gate; its prototype must remain opt-in and must not change the Direct Single
path.

## Rationale

Native fan-out provides a product-controlled path with no extra transcode. The
Delayed Program Source provides the broadest possible output-plugin
compatibility without requiring those plugins to expose their stream keys or
internal objects. Separating the two avoids making the dependable network path
wait on uncertain scene/audio capture work.

Long source delays must remain compressed. At 1920x1080 60 fps, 15 seconds of
NV12 frames is about 2.8 GB and RGBA is about 7.5 GB; an H.264 stream at 6 Mbps
is about 11.25 MB for the same duration before audio and container overhead.

## Trade-offs accepted

- Source compatibility mode performs an encode, decode, and the output
  plugin's encode. This costs GPU/CPU and can reduce quality.
- Native fan-out initially supports only destinations compatible with the same
  H.264/AAC rendition.
- A separate Delay Input scene and Broadcast Output scene may be required to
  prevent recursive capture.
- Vendor-specific seamless integration remains dependent on external APIs.

## Consequences

### Positive

- The current delay state machine can serve all output strategies.
- Network destinations can fail independently.
- A successful source mode will work with more than the two plugins currently
  under consideration.
- The architecture does not expose or scrape another plugin's credentials.

### Negative

- The pipeline needs explicit ownership, subscription, and lifecycle models.
- Source mode adds FFmpeg decoder, scaler, and resampler dependencies.
- Isolated scene audio may prove impractical with supported libobs APIs.
- UI states become more complex because the plugin may own media production but
  not the external broadcast connection.

### Mitigations

- Time-box the source feasibility spike and require objective pass/fail results.
- Keep modes explicit and mutually exclusive initially.
- Add synthetic flash/tone A/V tests and long-session drift measurement.
- Preserve the existing output until the new modes have runtime evidence.
- Fall back to a loopback feed and second OBS instance if in-process isolation
  cannot meet the acceptance criteria.

## Source feasibility gate

The gate below remains the technical standard. During implementation, satisfy
it with automated, synthetic, or agent-verifiable evidence where possible. Any
owner-operated steps remain pending until the combined final manual acceptance
gate; do not ask the owner for phase-by-phase tests.

The Delayed Program Source architecture may advance only if a prototype proves:

- an explicitly selected scene can be rendered without capturing the final
  scene containing the delayed source;
- programme audio for that selected scene can be captured and mixed without
  leaking unrelated sources;
- decoded video and audio can be emitted through an OBS async source with
  monotonic timestamps;
- a 15-second delay stays within 100 ms A/V offset during a 30-minute test;
- start, stop, source removal, collection change, and OBS shutdown are clean;
- recursion is detected and blocked; and
- measured resource use is documented and acceptable for the test system.

If isolated audio fails, the in-process source architecture is not accepted.
The next evaluated option is the two-OBS local bridge, not configuration
scraping or a normal-output handoff.

## Revisit triggers

Revisit this decision if:

- Aitum or StreamElements publishes a stable encoded-packet or destination API;
- OBS adds a supported programme-delay output stage shared by arbitrary outputs;
- isolated scene audio cannot be implemented with public libobs APIs;
- source-mode transcode cost is unacceptable on representative hardware; or
- supported platforms require different codecs/resolutions that native fan-out
  cannot share.
