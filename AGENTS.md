# Active Live Delay — agent guidance

## Scope and safety

- Windows x64 OBS Studio 32.2.1 plugin. Preserve the direct single-output path.
- Keep compatibility work behind explicit, mutually exclusive modes: Direct Single, Native Multistream, Compatibility Source.
- Never restore normal-OBS-output handoff; stopping it can end a live broadcast.
- Never log, commit, expose, or scrape stream keys, passwords, or third-party credentials.
- Preserve unrelated worktree changes. Do not delete releases/tags, close runtime-acceptance issues, commit, push, publish, or open PRs unless explicitly requested.
- Do not add dependencies, change public APIs/schemas/wire formats, or alter shared/production state without explicit approval.
- Keep user-visible errors and code professional; use stable non-secret diagnostic codes for new operational failures.

## Working rules

- Read relevant source, tests, docs, configuration, and nested `AGENTS.md` files before editing.
- Search for existing implementations first; make the smallest localized change that fully solves the request.
- Do not silently omit requested work, broaden scope, stack workaround layers, or leave replaced paths behind.
- Treat existing changes as user-owned; never reset, checkout, overwrite, or reformat unrelated work.
- Use `apply_patch` for edits. Prefer `rg`/`rg --files` for searches and the repository’s existing toolchain.
- State assumptions and blockers precisely. Ask one focused question only when no safe assumption can unblock work.
- For non-trivial work, delegate genuinely independent investigation or review when available; verify delegated claims independently.

## Architecture constraints

- Extension boundary is released encoded packets: `OBS H.264/AAC -> packet conversion -> DelayController -> FLV muxer -> bounded sender queue -> RTMP/RTMPS`.
- Keep long delays compressed; do not add long raw-video frame buffers.
- Native multistream needs independent bounded queues so a failed secondary cannot stall capture or primary.
- Delayed Program Source requires a feasibility spike for isolated scene video/audio, recursion prevention, lifecycle, and A/V sync.
- If supported libobs APIs cannot isolate scene audio, evaluate the loopback two-OBS bridge; never substitute global audio silently.

## Build and verification

```powershell
& "C:\Qt\Tools\CMake_64\bin\cmake.exe" -S . -B build-agent-core -DACTIVE_DELAY_BUILD_PLUGIN=OFF -DACTIVE_DELAY_BUILD_TESTS=ON
& "C:\Qt\Tools\CMake_64\bin\cmake.exe" --build build-agent-core --config Release
& "C:\Qt\Tools\CMake_64\bin\ctest.exe" --test-dir build-agent-core -C Release --output-on-failure
```

- Run the narrowest relevant automated tests, static checks, build, and `git diff --check`; report only checks actually run.
- The full plugin build needs local OBS SDK configuration. Prefer forward-slash CMake paths and never copy source from build directories.
- Build/unit-test success is not production acceptance. Runtime evidence is still required for delayed A/V, reconnect, long sessions, Return Live, Emergency Dump, stop, and OBS shutdown.
- Owner-operated manual/runtime testing remains deferred until all authorized implementation phases and automated checks finish; keep one combined final acceptance gate pending.

## Release and handoff

- Beta format: `v0.1.NN-betaNN`; align CMake version, README, warning banner, changelog, tag, prerelease title, and ZIP filename. Preserve earlier tags/releases.
- Final handoff states delivered behavior, changed files, checks/results, assumptions, limitations, and exact blockers. Never claim platform support, production readiness, or release acceptance without required runtime evidence.

## Key code areas

- `src/delay-controller.*`: timing, buffering, and state transitions.
- `src/released-packet-dispatcher.*`: immutable released-packet batches and consumer lifecycle.
- `src/flv-muxer.*`, `src/rtmp-sender.*`: direct/network delivery.
- `src/active-delay-output.*`, `src/active-delay-dock.*`: OBS pipeline and frontend lifecycle.
- `tests/`: core and OBS-linked regression tests.

## Commit attribution

- If commits are explicitly requested, AI commits must include `Co-Authored-By: GPT-5 <noreply@openai.com>`.
