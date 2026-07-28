# P4 isolated scene-capture feasibility probe

This is an opt-in, disposable OBS source used only to collect the evidence
required by P4. It is not the Delayed Program Source and must not be shipped
as a compatibility feature. It mirrors a selected **Delay Input** scene's
render and its public-libobs scene audio mix. It does not decode packets,
buffer raw frames, delay media, or open a network connection.

The normal plugin build leaves the probe out. Configure the dedicated probe
build with the same OBS SDK prefix and OBS CMake finder path used by the full
plugin build. In the maintained Windows environment:

```powershell
$obsSource = "C:/Users/AKS/Documents/OBS delay plugin/third_party/obs-studio"
$obsBuild = "$obsSource/build_x64_322"
$obsDeps = "$obsSource/.deps/obs-deps-2026-07-15-x64"
$obsQt = "$obsSource/.deps/obs-deps-qt6-2026-07-15-x64"
$prefix = "$obsBuild;$obsBuild/frontend/api;$obsBuild/deps/w32-pthreads;$obsDeps;$obsQt"

& "C:\Qt\Tools\CMake_64\bin\cmake.exe" -S . -B build-p4-probe `
  "-DCMAKE_PREFIX_PATH=$prefix" `
  "-DCMAKE_MODULE_PATH=$obsSource/cmake/finders" `
  "-DSIMDe_INCLUDE_DIR=$obsDeps/include" `
  -DACTIVE_DELAY_BUILD_PLUGIN=ON `
  -DACTIVE_DELAY_BUILD_TESTS=ON `
  -DACTIVE_DELAY_ENABLE_SCENE_PROBE=ON
& "C:\Qt\Tools\CMake_64\bin\cmake.exe" --build build-p4-probe --config Release
& "C:\Qt\Tools\CMake_64\bin\ctest.exe" --test-dir build-p4-probe -C Release --output-on-failure
```

Use a non-critical OBS collection. Do not configure or report stream keys,
passwords, or complete publish URLs.

## Isolation model

Create two separate scenes:

```text
Delay Input:      flash/tone source + nested scene + browser + mic + desktop audio
Broadcast Output: P4 Scene Capture Probe (Delay Input selected)
```

The probe uses `obs_source_video_render()` for the selected scene and
`obs_source_get_audio_mix()` for its scene-scoped mixed audio. It reports the
selected scene as an active and full-tree child. Libobs therefore rejects an
attempt to add the probe to its own selected scene, including through nested
scenes. The probe also checks this graph before each video or audio render and
blocks capture if a scene graph becomes recursive.

Never select a scene that contains the probe. Keep Delay Input and Broadcast
Output separate throughout the test.

## Evidence procedure

1. Add **Active Live Delay: P4 Scene Capture Probe** to Broadcast Output and
   choose Delay Input in its source properties.
2. Confirm the preview contains Delay Input, including a nested scene and a
   browser source. Change the Delay Input name, remove it, restore it, change
   scene collections, then stop and exit OBS. Record crashes, hangs, stale
   video, or `ALD-E5001` errors.
3. Put a microphone, desktop-audio source, browser audio, and one deliberately
   unrelated audio source in controlled scenes. Use OBS audio meters and a
   local non-critical recording to show that the selected scene mix is heard
   and the unrelated source is not. Record the exact scene layout and results.
4. Try direct recursion (add the probe to Delay Input) and nested recursion
   (put it in a child scene used by Delay Input). Both must be rejected or
   blocked without a crash. Record `ALD-E5002` and the OBS log.
5. For the 15-second flash/tone run, leave the probe only in Broadcast Output,
   use the existing direct delayed-output path, set a 15-second delay, and
   record independent playback for 30 minutes. At least every five minutes,
   record measured flash-to-tone offset; the absolute offset must remain at or
   below 100 ms. This is a runtime gate, not an automated-test result.
6. Record baseline and probe CPU, GPU, encoder, decoder, and memory use using
   the same OBS/profile/hardware. The probe has no decoder by design; record
   that value as `not applicable` rather than zero.

## Gate record

Do not mark P4 complete until this table is filled with saved logs/recordings.

| Gate | Result | Evidence location / notes |
|:---|:---|:---|
| Selected scene renders independently | pending | |
| Scene-scoped mix includes nested/browser/mic/desktop audio | pending | |
| Unrelated audio excluded | pending | |
| Direct and nested recursion blocked | pending | |
| Rename/removal/collection-change/OBS-exit lifecycle clean | pending | |
| 15-second flash/tone <= 100 ms for 30 minutes | pending | |
| CPU/GPU/encoder/decoder/memory measured | pending | |

If scene-scoped audio cannot be demonstrated through this public API, mark the
ADR gate failed and evaluate the loopback two-OBS bridge. Do not substitute
global desktop capture.
