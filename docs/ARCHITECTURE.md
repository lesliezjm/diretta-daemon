# Architecture - Diretta Host Daemon

## Overview

The daemon is structured around three layers:

1. **IPC Layer** (`IPCServer`) — Unix socket command interface
2. **Audio Layer** (`AudioEngine`) — FFmpeg decoding, format detection
3. **Diretta Layer** (`DirettaSync`) — SDK integration, lock-free ring buffer

```
IPCServer (epoll, JSON)
    │
    ├── onPlay / onPause / onStop / onSeek
    ├── onSelectTarget → disable() → enable() → warmup
    ├── onDiscoverTargets → static discoverTargets()
    └── notifyStateChange / notifyTrackChange / notifyPosition
            │
            ▼
DirettaRenderer (orchestrator)
    │
    ├── audioThreadFunc → AudioEngine → DirettaSync
    ├── positionThreadFunc → IPC notifyPosition every 1s
    └── lifecycle: open/close/stop/seek
            │
            ▼
AudioEngine (FFmpeg)
    ├── openTrack() → avformat_open_input / avcodec_open
    ├── readSamples() → PCM or DSD
    └── getState() / getPosition() / getDuration()
            │
            ▼
DirettaSync (SDK wrapper)
    ├── enable() → discoverTarget() → measureMTU() → openSyncConnection()
    ├── open(AudioFormat) → SDK setSink()
    ├── sendAudio() → ring buffer push → getNewStream() callback
    └── disable() → close() → shutdownWorker()
            │
            ▼
DIRETTA::Sync (Proprietary SDK)
    └── UDP packets → Diretta Target
```

## Threading Model

| Thread | Role | Lock-free? |
|--------|------|-----------|
| IPC event loop | Unix socket I/O, command dispatch | Yes (epoll) |
| Audio thread | Decode → ring buffer → SDK callback | Partial (atomic guards) |
| Position thread | Push position events to IPC clients | Yes (try_lock) |

**The audio hot path is lock-free.** `sendAudio()` uses `RingAccessGuard` (atomic increment) to access the ring buffer. No mutex in the hot path.

## Key Design Decisions

### 1. Runtime Target Selection

The daemon does not bind to a target at startup. Instead:
- `discoverTargets()` scans the network using `DIRETTA::Find`
- `select_target` IPC command triggers `disable()` → `setTargetIndex()` → `enable()` + warmup
- This allows a single daemon to serve multiple targets, switching between them

### 2. Warmup Pre-connect

After connecting to a target, the daemon opens a dummy 44.1kHz/24-bit stream and immediately closes it. This warms up the Diretta pipeline and eliminates the ~5 second glitch on the first real playback.

### 3. Lifecycle Mutex

`m_lifecycleMutex` (recursive) protects `open()`/`close()`/`stop()`/`release()` from concurrent access. The audio callback can call `stopPlayback()` while the IPC thread is in `open()` doing a format transition.

### 4. Reconfiguration Guard

When the format changes (PCM↔DSD or sample rate change), the ring buffer is reconfigured. `beginReconfigure()` waits for all active `RingAccessGuard` users to drain, then swaps the buffer. No locks in the hot path.

## Audio Hot Path

```
AudioEngine::readSamples()
    │
    ├─ PCM: av_read_frame() → swr_convert() → ring buffer
    │
    └─ DSD: av_read_frame() → byte swap / bit reverse → ring buffer
                │
                ▼
        DirettaSync::sendAudio()
            │
            ├─ RingAccessGuard (atomic, non-blocking)
            ├─ Format conversion (AVX2 / NEON / scalar)
            └─ memcpy_audio_fixed() to ring (power-of-2 mask)

DirettaSync::getNewStream() [SDK thread, hot path]
            │
            ├─ RingAccessGuard (atomic, non-blocking)
            └─ memcpy_audio() to DIRETTA::Stream
```

## Lock-Free Ring Buffer

The `DirettaRingBuffer` uses a power-of-2 sized buffer with bitmask modulo:
```cpp
size_t mask_ = size_ - 1;      // e.g., 4096 → 0xFFF
pos_ = (pos_ + n) & mask_;    // 1-cycle modulo vs 10-20 for %
```

**Write position** is atomic (`std::atomic<size_t>`). **Read position** is guarded by `RingAccessGuard`. Cache-line separation (`alignas(64)`) prevents false sharing.

## Format Transitions

| From | To | Action | Delay |
|------|-----|--------|-------|
| PCM | Same PCM (same rate) | Buffer clear, quick resume | None |
| PCM | PCM (rate change) | `close()` + `open()` | 200ms |
| PCM | DSD | `reopenForFormatChange()` | 800ms |
| DSD | Same DSD (same rate) | Buffer clear, quick resume | None |
| DSD | DSD (rate change) | `close()` + `open()` | 400ms |
| DSD | PCM | `close()` + `open()` | 800ms |

## Files

| File | Layer | Hot Path? |
|------|-------|-----------|
| `src/IPCServer.cpp` | IPC | No |
| `src/DirettaRenderer.cpp` | Orchestrator | Partial |
| `src/AudioEngine.cpp` | Audio decode | No |
| `src/DirettaSync.cpp` | SDK wrapper | **Yes** |
| `src/DirettaRingBuffer.h` | Ring buffer | **Critical** |
| `src/fastmemcpy-avx.c` | SIMD memcpy | **Critical** |
| `src/glibc_compat.c` | ABI shim | No |

## What Was Removed (v3.0)

The following UPnP/DLNA components were removed:
- `src/UPnPDevice.hpp/cpp` — UPnP device, SSDP discovery, SOAP/HTTP server
- `src/ProtocolInfoBuilder.h` — UPnP protocol info generation
- `src/UPnPControlPoint.cpp/hpp` — UPnP control point
- All `libupnp` and `libixml` linking

The Diretta SDK integration (`DirettaSync`, `DirettaRingBuffer`, `AudioEngine`) was **preserved exactly** — no changes to the audio hot path.
