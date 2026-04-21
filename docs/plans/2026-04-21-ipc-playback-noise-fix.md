# IPC Playback Noise Fix Plan

## Summary

- True clone baseline: `68c120a`, cloned from `https://github.com/cometdom/DirettaRendererUPnP.git` on `2026-04-08 08:59:50 +0800`.
- Current IPC refactor commit `e3d87f7` collapsed the original UPnP `SetAVTransportURI`, `SetNextAVTransportURI`, and `Play` flow into `play(path)`.
- That changes playback semantics: when a client sends the next track while playback is active, the current code auto-stops and reopens instead of using `AudioEngine::setNextURI()` preload/gapless behavior.
- First-play noise remains the first priority; next-track behavior must also be corrected because the Go client currently sends `play(nextPath)` while playing.

## Key Changes

- Extend IPC with explicit commands:
  - `set_uri`: set current track without starting playback.
  - `queue_next`: queue/preload the next track through `AudioEngine::setNextURI()`.
  - `play_now`: immediately replace current playback with a new path.
- Keep backward compatibility for existing `play(path)` clients:
  - stopped or paused with a path: set current URI and start/resume playback.
  - playing with a different path: queue it as next track instead of auto-stopping.
  - playing with the same path: no-op.
  - no path: resume.
- Align renderer behavior with the original UPnP callbacks:
  - Split URI setting, next URI queueing, resume, and immediate replacement paths.
  - Preserve the original auto-stop + silence + reopen path only for explicit replacement.
  - Record `m_lastStopTime` after warmup so first real playback still gets DAC stabilization delay.
- Reduce IPC event impact on audio callbacks:
  - Avoid blocking audio-related callbacks on socket send paths where practical.
- Restore upstream stability options from the working reference:
  - `--cpu-audio` and `--cpu-other` CLI flags.
  - `DirettaConfig::cpuAudio/cpuOther` and SDK/worker CPU pinning.
  - Keep user-local Makefile changes intact unless needed for build correctness.

## Test Plan

- Build checks:
  - `make test`
  - `make`
- Startup/selection model:
  - Start daemon without `--target`; runtime clients choose the output device
    with `discover_targets` and `select_target`.
- IPC protocol checks:
  - `discover_targets -> acquire_control -> select_target -> set_uri(path) -> play` starts first playback with warmup and stabilization.
  - Manual `socat` checks keep the connection open briefly with `sleep`; plain `printf ... | socat` can close before discovery or open/play responses are returned.
  - while playing, `play(nextPath)` logs `Next URI queued` / `Anticipated preload started`, not `Auto-STOP before new track`.
  - `play_now(path)` performs explicit stop/reopen replacement.
  - same-path `play(path)` during playback is ignored.
- Audio scenarios:
  - first playback after target selection.
  - PCM same-format track transition.
  - PCM sample-rate/bit-depth transition.
  - DSD transition if available.
  - pause/resume and stop/play.

## Follow-Up Observations

- Local file playback is currently logged as `Remote server - using streaming
  options (reconnect enabled)`. This is a source-classification issue in
  `AudioDecoder::open()`: plain local paths are falling into the non-local-server
  branch. The playback run was stable because that branch mainly uses larger
  buffering, but the log label is misleading and should be cleaned up later.
- `status.path` originally returned empty because `TrackInfo::uri` was not
  populated after opening the track. This was fixed by copying the active URI and
  metadata into `m_currentTrackInfo` after `AudioDecoder::open()` and after
  gapless track promotion.

## Assumptions

- Keep socket + Diretta Host SDK architecture; do not restore UPnP.
- Existing Go backend may continue sending `play(nextPath)` while playback is active.
- Current uncommitted `.gitignore` and `Makefile` edits are user context and should not be reverted.
