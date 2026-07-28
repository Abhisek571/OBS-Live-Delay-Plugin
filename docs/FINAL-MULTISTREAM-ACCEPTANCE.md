# Final combined multistream acceptance

Run this owner-operated matrix once, after the automated implementation gates
pass. Use non-critical Twitch, YouTube, and Kick broadcasts. This workflow is
runtime evidence; builds and unit tests are not substitutes.

Never include a stream key, password, server URL, or complete publish URL in
notes, screenshots, copied diagnostics, or OBS logs shared with others.

## Setup

- OBS Studio 32.2.1 on Windows x64.
- Simple Output, H.264 video, AAC audio, CBR, two-second keyframes.
- No more than 1920x1080 at 60 fps and no more than 8,000 kbps video so the
  shared rendition stays within Kick's documented limits.
- Primary service configured in OBS Settings -> Stream.
- Destination 2 and destination 3 configured from the current official server
  URL and key shown by their platform dashboards.
- A visible millisecond clock and regular audio cue in the programme for delay,
  inter-destination presentation, and A/V checks.
- A safe OBS log collection location with no credentials.

Do not press OBS Start Streaming. The Active Live Delay dock owns these
connections.

## Direct Single

1. Disable both secondary cards.
2. Confirm Preflight shows one enabled destination.
3. Start Broadcast and confirm only the primary is live.
4. Set a 15-second delay. Confirm the requested delay is within +/- 250 ms after
   stabilisation and A/V remains synchronised.
5. Increase the delay, then Return Live (Keep Broadcasting). Confirm the primary broadcast stays
   connected.
6. End Broadcast. Confirm the warning explains that the broadcast will end,
   then confirm stop.

## Native Multistream

1. Enable destination 2 and destination 3. Select the correct platform label
   for each and paste the current official server URL and key.
2. Confirm keys are masked. Press and hold each reveal control, then release it;
   confirm masking returns immediately.
3. Confirm Preflight reports three enabled destinations and a credible total
   upload estimate. Resolve every hard `ALD-E2016` issue before continuing.
4. Start Broadcast. Confirm all three destination rows reach ACTIVE and show
   independent reconnect, queue, and sent-byte values.
5. Confirm Twitch, YouTube, and Kick show the same programme and audio. Measure
   inter-destination presentation difference; it must be no more than 250 ms.
6. Set a 15-second delay. Confirm all destinations change together, the final
   delay is within +/- 250 ms, and A/V remains synchronised.
7. Return Live (Keep Broadcasting). Confirm all three broadcasts remain connected.
8. Interrupt one secondary network path without interrupting the primary or
   other secondary. Confirm the affected row reports failure/reconnect while
   both healthy destinations continue. Restore the path and confirm keyframe-
   aligned recovery.
9. Run 30-minute focused checks, then a two-hour stability session. Record
   initial and final A/V offset; drift change must be no more than 50 ms. Watch
   for unbounded queue or memory growth.
10. End Broadcast and confirm all three broadcasts end cleanly.
11. Repeat startup, then exit OBS while active. Confirm clean shutdown with no
    crash, hang, stuck worker, or credential in the OBS log.

## Evidence record

Record only safe values:

- date/time, OBS version, plugin build hash, encoder, resolution, fps, and
  configured video/audio bitrate;
- platform names without channel credentials;
- measured delay, presentation difference, A/V offset, drift, duration, and
  CPU/GPU/memory observations;
- actions taken and each destination's safe state;
- any `ALD-E####` code and its safe message;
- result for stop and OBS shutdown.

Keep the release beta and platform-support claims pending if any required item
fails or lacks evidence.

Current official setup references:

- [Kick OBS setup and limits](https://help.kick.com/en/articles/7066931-how-to-stream-on-kick-com)
- [YouTube Live encoder settings](https://support.google.com/youtube/answer/2853702)
- [Twitch stream-key guidance](https://help.twitch.tv/s/article/twitch-stream-key-faq)
