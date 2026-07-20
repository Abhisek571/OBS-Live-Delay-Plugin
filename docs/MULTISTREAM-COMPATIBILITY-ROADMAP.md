# Multistream and output-plugin compatibility roadmap

Status: design backlog; no compatibility work described here is implemented.

This roadmap expands [ADR-001](architecture/ADR-001-multistream-compatibility.md)
into implementation-sized phases. It is intentionally gated: the current
v0.1.39 direct single-output beta remains the baseline until each new mode has
automated and runtime evidence.

## Product outcomes

The completed work should provide three explicit operating modes:

1. **Direct Single Output** — the existing plugin-owned RTMP/RTMPS path.
2. **Native Multistream (experimental)** — one delayed H.264/AAC rendition sent to several
   plugin-owned destinations.
3. **Compatibility Source** — a normal OBS A/V source containing the delayed
   programme, allowing normal OBS, Aitum, SE.Live, or another output owner to
   stream it.

Mode selection must be explicit. MVP implementations must not run Direct Output
and Compatibility Source concurrently.

## Invariants

All implementation phases must preserve these rules:

- Never log a complete stream key, password, or publish URL containing a key.
- Never stop an external/normal OBS output as a handoff mechanism.
- Consume each released packet batch from `DelayController` exactly once.
- Resume video after discontinuity or reconnect only on a keyframe.
- Keep audio and video on one monotonic output epoch.
- A slow secondary destination must not block capture or a healthy primary.
- Stop and shutdown paths must be bounded and must not execute UI work from a
  media callback thread.
- The current single-output mode must continue to pass its existing tests.

## Proposed modules and interfaces

Names are provisional but describe the intended responsibilities.

### `OperatingMode`

```cpp
enum class OperatingMode {
    DirectSingle,
    NativeMulti,
    CompatibilitySource,
};
```

Stored in plugin settings, changed only while stopped, and surfaced prominently
in the dock.

### `ReleasedPacketDispatcher`

Responsibilities:

- call `DelayController::take_ready_packets()` once per ingest callback;
- wrap the moved vector in an immutable released batch;
- publish the batch to the configured consumer;
- carry discontinuity and epoch-reset events alongside media batches; and
- expose queue depth and dropped/failure state without copying payloads for each
  network destination.

Suggested event model:

```cpp
struct ReleasedPacketBatch {
    std::uint64_t epoch = 0;
    std::vector<EncodedPacket> packets;
};

struct DiscontinuityEvent {
    std::uint64_t epoch = 0;
    std::string reason;
};

class IReleasedPacketConsumer {
public:
    virtual ~IReleasedPacketConsumer() = default;
    virtual bool start(const FlvCodecHeaders &, std::string &error) = 0;
    virtual void consume(std::shared_ptr<const ReleasedPacketBatch>) = 0;
    virtual void discontinuity(const DiscontinuityEvent &) = 0;
    virtual void stop() noexcept = 0;
};
```

Do not introduce a generic event framework. One concrete dispatcher and narrow
consumer interface are sufficient.

### `NetworkPacketConsumer`

Responsibilities:

- own one `FlvMuxer` per encoded rendition;
- mux each released packet batch once;
- serialize immutable FLV headers/tags once where possible; and
- publish the resulting batch to `MultiTargetSender`.

### `MultiTargetSender`

Responsibilities:

- own a `TargetWorker` for every enabled destination;
- connect targets concurrently without blocking the Qt UI thread;
- give every target a bounded independent queue;
- maintain per-target state, error, bytes, queue depth, and reconnect count;
- isolate secondary failure;
- prime each connection with FLV and codec sequence headers; and
- discard until a video keyframe after connect/reconnect.

Suggested state model:

```cpp
enum class TargetState {
    Disabled,
    Starting,
    Running,
    Reconnecting,
    Failed,
    Stopping,
    Stopped,
};

struct TargetStatus {
    std::string id;
    TargetState state;
    bool primary;
    std::size_t queued_bytes;
    std::uint64_t sent_bytes;
    std::uint32_t reconnects;
    std::string redacted_error;
};
```

MVP failure policy:

- secondary startup/runtime failure: mark only that target failed and continue;
- primary startup failure: fail the overall start before declaring ACTIVE;
- primary runtime failure: show an output error, keep healthy secondaries alive,
  and require the user to choose whether to stop all;
- all targets failed: stop capture and move the plugin output to ERROR;
- queue overflow: fail/reconnect only the affected target; never block ingest.

Live target addition/removal is out of scope for the MVP. Configuration controls
remain disabled while active.

### `DestinationStore`

Responsibilities:

- treat the current OBS frontend service as the primary destination;
- store additional RTMP/RTMPS destinations with stable IDs and display names;
- validate protocols and required fields;
- redact secrets from logs, status objects, diagnostics, and copied errors;
- migrate settings by version; and
- provide an explicit reveal/copy interaction rather than displaying keys by
  default.

Do not read Aitum or SE.Live private configuration files. Do not import secrets
without a documented provider API and explicit user action.

### `DelayedProgramSource`

Register an OBS input source with asynchronous video and audio capabilities.
The source receives decoded frames from a decoder worker and emits them using
`obs_source_output_video2()` and `obs_source_output_audio()`.

Responsibilities:

- expose width, height, frame rate, audio layout, and readiness;
- emit a holding frame and silence while priming;
- keep decoded A/V timestamps monotonic across controller epochs;
- stop producing frames promptly when removed or when the producer stops;
- reject multiple incompatible producers; and
- publish source health to the dock without calling Qt from its worker.

Use `OBS_SOURCE_DO_NOT_DUPLICATE` initially so scene duplication cannot create
independent decoders accidentally.

### `DelayedAvDecoder`

FFmpeg-backed decoder responsibilities:

- initialize H.264 and AAC decoders from cached codec headers;
- accept the plugin's AVC-form H.264 and AAC access units;
- flush on discontinuity, Close Delay jumps, and encoder reconfiguration;
- honour decoder frame reordering and use best-effort presentation timestamps;
- convert video to an OBS-supported format such as NV12 or I420;
- resample audio to the active OBS sample rate/layout; and
- output video/audio frames through bounded queues.

The build will likely need FFmpeg `avcodec`, `swscale`, and `swresample` in
addition to the current `avformat` and `avutil` libraries.

### `IsolatedSceneCapture`

This is a feasibility-spike component, not an assumed capability.

Responsibilities under investigation:

- retain a weak reference to the selected Delay Input scene;
- render it through an isolated OBS view or supported source-render path;
- capture/mix only audio belonging to that scene;
- feed video/audio to the existing H.264/AAC encoder path;
- continue while the Delay Input scene is not the frontend programme scene;
- detect direct and nested references to the Delayed Program Source; and
- tear down safely on scene removal, collection change, or frontend exit.

No source MVP begins until this component passes its feasibility gate.

## Implementation phases

### Phase 0 — preserve and characterize v0.1.39

- Complete issue #5's recorded non-critical runtime acceptance.
- Save baseline logs and resource usage for single-output mode.
- Add characterization tests around delay state transitions, timestamp epochs,
  packet release order, and output failure signaling.
- Document the exact OBS 32.1.2, encoder, service, and FFmpeg versions used.

Exit criteria:

- the current path has a known-good runtime baseline;
- existing five CTest targets pass freshly; and
- no compatibility refactor begins from an unknown runtime state.

### Phase 1 — introduce mode and lifecycle boundaries

- Add `OperatingMode` and a stopped-only mode selector.
- Split `ActiveDelaySession` into controller state, codec configuration, and
  consumer/lifecycle state rather than adding more fields to the current class.
- Define one owner for capture, one owner for the active consumer, and explicit
  start/stop ordering.
- Move user-visible output status to immutable snapshots polled by the dock.
- Add state-transition tests for start cancellation, partial start, stop during
  start, frontend exit, and repeated clicks.

Exit criteria:

- Direct Single Output behaves as before;
- modes cannot overlap; and
- no media callback reaches released UI objects.

### Phase 2 — released-packet dispatcher

- Introduce `ReleasedPacketDispatcher` after the delay controller.
- Move the current mux/send code behind `NetworkPacketConsumer`.
- Carry explicit epoch/discontinuity events.
- Preserve payload ownership with move/shared immutable batches.
- Add fake consumers and verify ordering, single consumption, and shutdown.

Exit criteria:

- existing output produces byte-equivalent FLV for deterministic fixtures;
- packet payloads are not copied once per destination; and
- consumer failure cannot corrupt controller state.

### Phase 3 — native fan-out MVP

- Generalize `RtmpSender` into reusable target workers.
- Add `MultiTargetSender` coordination and independent queues.
- Add primary plus manual secondary destination configuration.
- Connect targets away from the UI thread.
- Add per-target dock state and aggregate output state.
- Disable destination editing while active.

Exit criteria:

- two fake destinations receive identical FLV tag order and timestamps;
- one slow/failing destination does not stall the other;
- reconnect begins on a keyframe with fresh sequence headers; and
- secret-redaction tests cover every error/status path.

### Phase 4 — isolated scene-capture spike

Build a disposable or feature-flagged prototype before the full source UI.

- Select one scene as Delay Input.
- Render it independently of the frontend programme scene.
- Capture a flash-frame plus tone test pattern.
- Capture nested scene sources, browser audio, microphone, and desktop audio.
- Prove that a Broadcast Output scene can contain the delayed source without
  feeding that output back into Delay Input.
- Exercise scene removal, rename, collection change, and shutdown.
- Record CPU, GPU, encoder, decoder, memory, and timestamp data.

Pass criteria:

- isolated video and scene-scoped audio are both available through supported
  libobs APIs;
- no recursive frame path exists;
- 15-second A/V offset is within 100 ms for 30 minutes;
- no crash/hang occurs across the lifecycle matrix; and
- resource measurements are recorded.

Fail decision:

- if scene-scoped audio cannot be isolated, stop in-process source work and
  evaluate the two-OBS local bridge;
- do not replace audio with global desktop capture silently.

### Phase 5 — decoder and source MVP

Begins only if Phase 4 passes.

- Add FFmpeg decoder/scaler/resampler dependencies.
- Implement decoder unit tests using small generated H.264/AAC fixtures.
- Register `active_delay_program_source`.
- Add holding-frame/silence priming behaviour.
- Add source-ready, source-error, and buffered-duration status.
- Require the source to report READY before external Start Streaming.
- Flush and reacquire a keyframe after discontinuities.

Exit criteria:

- normal OBS can stream the Delayed Program Source through its standard output;
- stopping the source producer does not crash or hang OBS;
- delay changes and Close Delay preserve monotonic A/V output;
- a two-hour synthetic session remains within the agreed A/V drift tolerance.

### Phase 6 — Aitum compatibility acceptance

- Pin and record the tested Aitum version/commit.
- Configure at least two non-critical destinations.
- Start Aitum only after Delayed Program Source reports READY.
- Verify identical delay and content at both destinations.
- Exercise one destination failure/reconnect while the other continues.
- Exercise delay increase/decrease, Close Delay, stop, and OBS
  shutdown.
- Save OBS logs and independent platform recordings.

Passing this phase means source-based compatibility with the tested Aitum
version. It does not imply use of or support for Aitum's private API.

### Phase 7 — SE.Live compatibility acceptance

- Pin and record the tested SE.Live version.
- Verify the Delayed Program Source through its normal multistream workflow.
- Verify StreamElements browser overlays and alerts are included in Delay Input.
- Check that SE.Live controls do not stop the delay producer unexpectedly.
- Repeat lifecycle, delay, sync, failure, and shutdown checks.

Passing this phase applies only to the tested SE.Live/OBS combination. If
StreamElements exposes a supported packet/destination API later, evaluate it as
a separate adapter ADR.

### Phase 8 — optional local bridge fallback

If Phase 4 fails, prototype a loopback-only bridge:

```text
OBS A + Active Live Delay -> local SRT/RTMP listener -> OBS B media source
OBS B -> Aitum or SE.Live -> platforms
```

- bind only to loopback by default;
- require an unpredictable local token where the protocol supports it;
- provide clear two-instance profile instructions;
- verify start ordering and reconnection; and
- document the extra decode/re-encode and operational cost.

Do not embed and expose a general unauthenticated RTMP server as a shortcut.

### Phase 9 — supported vendor adapters

- Ask Aitum for a documented cross-plugin integration point.
- Ask StreamElements for a supported encoded-source or destination API.
- Prefer a versioned API negotiated through OBS procedure/signal facilities.
- Feature-detect adapters at runtime and fail closed when versions differ.
- Keep native fan-out and Compatibility Source independent of adapters.

## UI plan

The dock should separate producer, delay, and delivery state:

```text
Mode: [Compatibility Source v]

Producer
  Delay Input: [Scene name v]     State: READY
  [Start Producer] [Stop Producer]

Delay
  Target: [15] seconds            Current: 15.0 s
  [Enable Delay] [Close Delay]

Delivery
  Compatibility Source: ACTIVE
  External output: OBS/Aitum/SE.Live (informational if detectable)
```

For Native Multistream, Delivery becomes a per-target table. Colour may
supplement but never replace state text. Stop Producer and Stop All Destinations
must be clearly different actions and require confirmation when they end a live
broadcast.

## Testing strategy

### Unit tests

- released packet ordering and exactly-once controller drain;
- shared batch lifetime across consumers;
- target queue isolation and overflow;
- concurrent startup and cancellation;
- primary/secondary failure policy;
- redaction of URLs, keys, usernames, and passwords;
- epoch/discontinuity propagation;
- decoder flush and keyframe reacquisition;
- timestamp conversion, B-frame reorder, audio resampling;
- recursive scene graph detection;
- mode and lifecycle state transitions.

### Component tests

- fake RTMP servers with independent latency/failure injection;
- deterministic FLV comparison against the current single output;
- generated flash/tone A/V fixtures;
- asynchronous OBS source output under normal, jittered, and discontinuous
  timestamps;
- selected-scene removal and collection replacement;
- repeated load/unload and frontend shutdown.

### Runtime matrix

Minimum recorded matrix:

- OBS Studio 32.1.2 on Windows x64;
- software H.264 and the supported hardware encoder used by the maintainer;
- Direct Single, Native Multi, and Compatibility Source modes;
- normal OBS output, tested Aitum build, and tested SE.Live build;
- zero delay, 15 seconds, increased delay, reduced delay, and Close Delay;
- network interruption of one destination;
- 30-minute focused sessions and a two-hour stability session;
- stop output, remove source, change collection, and exit OBS.

### Approved measurable thresholds

The owner approved these baseline acceptance thresholds for gated compatibility
work on 2026-07-21. Later revisions must be explicit and evidence-based; a gate
must not be silently weakened to make a failing result pass:

- inter-destination presentation difference: <= 250 ms;
- source-mode A/V offset: <= 100 ms;
- two-hour A/V drift change: <= 50 ms from the initial measured offset;
- no unbounded queue or memory growth;
- no complete secret appears in an OBS log;
- healthy destination continues through another destination's failure;
- shutdown completes without crash or stuck worker;
- requested 15-second delay is within +/- 250 ms after stabilization.

## Issue breakdown

Create separate issues rather than one unreviewable implementation issue:

1. [#6 Pipeline characterization and baseline acceptance](https://github.com/Abhisek571/OBS-Live-Delay-Plugin/issues/6).
2. [#7 Operating modes and lifecycle refactor](https://github.com/Abhisek571/OBS-Live-Delay-Plugin/issues/7).
3. [#8 Released-packet dispatcher and network consumer boundary](https://github.com/Abhisek571/OBS-Live-Delay-Plugin/issues/8).
4. [#9 Native `MultiTargetSender` core](https://github.com/Abhisek571/OBS-Live-Delay-Plugin/issues/9).
5. [#10 Native multistream destination storage and dock UI](https://github.com/Abhisek571/OBS-Live-Delay-Plugin/issues/10).
6. [#11 Isolated scene video/audio feasibility spike](https://github.com/Abhisek571/OBS-Live-Delay-Plugin/issues/11).
7. [#12 FFmpeg delayed H.264/AAC decoder](https://github.com/Abhisek571/OBS-Live-Delay-Plugin/issues/12).
8. [#13 Delayed Program Source registration and lifecycle](https://github.com/Abhisek571/OBS-Live-Delay-Plugin/issues/13).
9. [#14 Aitum compatibility acceptance](https://github.com/Abhisek571/OBS-Live-Delay-Plugin/issues/14).
10. [#15 SE.Live compatibility acceptance](https://github.com/Abhisek571/OBS-Live-Delay-Plugin/issues/15).
11. [#16 Optional loopback two-OBS bridge spike](https://github.com/Abhisek571/OBS-Live-Delay-Plugin/issues/16).
12. [#17 Supported Aitum and StreamElements API discussions and adapters](https://github.com/Abhisek571/OBS-Live-Delay-Plugin/issues/17).

Every issue should contain Problem, Goal, Scope, Non-Goals, Dependencies,
testable Acceptance Criteria, Status, and Execution Gate. Runtime-dependent
issues must remain open until logs and independent playback evidence are
attached.

## Explicit non-goals for the first multistream release

- Different video renditions or codecs per native destination.
- Enhanced Broadcasting or Twitch multitrack integration.
- Live editing/addition/removal of destinations.
- Scraping credentials from Aitum or SE.Live.
- Manipulating OBS's native status bar through unsupported Qt hooks.
- Claiming compatibility with untested plugin versions.
- Production-ready status based only on unit tests.
