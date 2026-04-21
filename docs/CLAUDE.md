# CLAUDE.md - Diretta Host Daemon

## Overview

Diretta Host Daemon v3.0 — a native Linux daemon that streams high-resolution audio to Diretta Targets via Unix socket IPC. Replaces the UPnP/DLNA layer from v2.1.x with a clean JSON-over-Unix-socket interface for external music player integration (Go, etc.).

**Core principle: Never touch Diretta SDK integration code (DirettaSync, DirettaRingBuffer). The audio hot path, format negotiation, flow control — all preserved exactly.**

## Architecture

```
IPCServer (epoll, JSON)
    │
    ├─ discover_targets → static DirettaSync::discoverTargets()
    ├─ select_target    → disable() → setTargetIndex() → enable() → warmup
    ├─ play / pause / stop / seek
    └─ notifyStateChange / notifyTrackChange / notifyPosition (broadcast)
            │
            ▼
DirettaRenderer (orchestrator)
    ├─ audioThreadFunc   → flow control, idle release timer
    ├─ positionThreadFunc → position push every 1s
    └─ lifecycle mutex: open/close/stop/release
            │
            ▼
AudioEngine (FFmpeg) + DirettaSync (SDK)
    │
    └─ getNewStream() callback → lock-free ring buffer
            │
            ▼ UDP/Ethernet
Diretta Target (DAC)
```

## Key Files

| File | Purpose | Hot Path? |
|------|---------|-----------|
| `src/IPCServer.cpp/h` | Unix socket IPC, JSON protocol, command dispatch | No |
| `src/DirettaRenderer.cpp/h` | Orchestrator: threading, lifecycle, callbacks | Partial |
| `src/DirettaSync.cpp/h` | SDK wrapper, ring buffer, format negotiation | **Yes** |
| `src/DirettaRingBuffer.h` | Lock-free SPSC, AVX2/NEON format conversion | **Critical** |
| `src/AudioEngine.cpp/h` | FFmpeg decode, format detection | No |
| `src/glibc_compat.c` | glibc 2.38 ABI shim | No |
| `src/fastmemcpy-avx.c` | C AVX memcpy | **Critical** |

## IPC Protocol

**Socket:** `/tmp/diretta-renderer.sock` (configurable)

Commands: `discover_targets`, `status`, `acquire_control`, `release_control`,
`set_uri`, `queue_next`, `play`, `play_now`, `pause`, `stop`, `seek`,
`select_target`, `shutdown`

Full protocol: [docs/IPC_PROTOCOL.md](IPC_PROTOCOL.md)

## Key Design Decisions

### 1. Runtime Target Selection

Daemon starts without a bound target. `select_target` IPC command triggers:
```
disable() → setTargetIndex() → enable() → warmup
```
This allows one daemon to serve multiple targets, switching at runtime.

### 2. Warmup Pre-connect

After connecting to a target, the daemon opens a dummy 44.1kHz/24-bit stream and closes it immediately. This warms up the Diretta pipeline and eliminates the ~5s glitch on first play.

### 3. Lifecycle Mutex

`m_lifecycleMutex` (recursive) protects `open()`/`close()`/`stop()`/`release()` from concurrent access. The audio callback can call `stopPlayback()` while the IPC thread is in `open()` doing a format transition.

### 4. Reconfiguration Guard

Format changes (PCM↔DSD, sample rate change) reconfigure the ring buffer. `beginReconfigure()` waits for all `RingAccessGuard` users to drain, then swaps the buffer. No locks in the hot path.

## Audio Hot Path

```
AudioEngine::readSamples()
    └─▶ DirettaSync::sendAudio()
            ├─ RingAccessGuard (atomic, non-blocking)
            ├─ Format conversion (AVX2/NEON/scalar)
            └─ memcpy_audio_fixed() to ring (power-of-2 mask)

DirettaSync::getNewStream() [SDK thread]
            ├─ RingAccessGuard (atomic, non-blocking)
            └─ memcpy_audio() to DIRETTA::Stream
```

## Build & Run

```bash
make clean && make
sudo ./bin/DirettaRenderer                    # no target, use IPC
sudo ./bin/DirettaRenderer --verbose          # debug logs
```

Manual playback flow:

```bash
(printf '%s\n' '{"cmd":"discover_targets"}'; sleep 2) \
| sudo socat -T 5 - UNIX-CONNECT:/tmp/diretta-renderer.sock

(printf '%s\n%s\n' \
  '{"cmd":"acquire_control"}' \
  '{"cmd":"select_target","target":"1"}'; sleep 3) \
| sudo socat -T 12 - UNIX-CONNECT:/tmp/diretta-renderer.sock

(printf '%s\n%s\n' \
  '{"cmd":"acquire_control"}' \
  '{"cmd":"set_uri","path":"/path/to/test.wav"}'; sleep 2) \
| sudo socat -T 8 - UNIX-CONNECT:/tmp/diretta-renderer.sock

(printf '%s\n%s\n' \
  '{"cmd":"acquire_control"}' \
  '{"cmd":"play"}'; sleep 5) \
| sudo socat -T 10 - UNIX-CONNECT:/tmp/diretta-renderer.sock
```

## SDK Reference

**SDK Location:** `../DirettaHostSDK_148/` (v148)

### Thread Modes (`DIRETTA::Sync::THRED_MODE`)
```
CRITICAL = 1       NOSHORTSLEEP = 2    NOSLEEP4CORE = 4
OCCUPIED = 16      NOSLEEPFORCE = 2048  NOJUMBOFRAME = 8192
```

### Format Bitmasks (`Format.hpp`)
- PCM: `FMT_PCM_SIGNED_16` (0x0200), `FMT_PCM_SIGNED_24` (0x0400), `FMT_PCM_SIGNED_32` (0x0800)
- DSD: `FMT_DSD1` (0x010000), `FMT_DSD_LSB` (0x100000), `FMT_DSD_MSB` (0x200000)
- Sample rates: `RAT_44100` (0x0200_00000000) through `RAT_MP4096`

## What Was Removed (v3.0)

All UPnP/DLNA code:
- `src/UPnPDevice.hpp/cpp` — removed
- `src/ProtocolInfoBuilder.h` — removed
- `src/UPnPControlPoint.cpp/hpp` — removed
- `libupnp` / `libixml` dependencies — removed

## Format Transitions

| From | To | Action | Delay |
|------|-----|--------|-------|
| PCM | Same PCM (same rate) | Buffer clear, quick resume | None |
| PCM | PCM (rate change) | `close()` + `open()` | 200ms |
| PCM | DSD | `reopenForFormatChange()` | 800ms |
| DSD | Same DSD (same rate) | Buffer clear, quick resume | None |
| DSD | DSD (rate change) | `close()` + `open()` | 400ms |
| DSD | PCM | `close()` + `open()` | 800ms |

## Coding Conventions

- **Language:** C++17
- **Member prefix:** `m_`
- **Atomics:** Use `std::memory_order_acquire`/`release`
- **Alignment:** `alignas(64)` for cache-line separation
- **Indentation:** 4 spaces, max 120 chars

## Working with This Codebase

1. **Check if hot path** — `DirettaRingBuffer`, `sendAudio()`, `getNewStream()` need extra scrutiny
2. **Test with DSD** — DSD is more timing-sensitive than PCM
3. **Verify lock-free** — No mutex in audio path
4. **Test format transitions** — PCM↔DSD transitions are most problematic

## Documentation

| Document | Purpose |
|----------|---------|
| `README.md` | Quick start |
| `docs/ARCHITECTURE.md` | System architecture |
| `docs/IPC_PROTOCOL.md` | IPC command reference |
| `docs/GO_INTEGRATION.md` | Go client guide |
| `docs/CONFIGURATION.md` | All config options |
| `docs/INSTALLATION.md` | Build and systemd setup |
| `docs/TROUBLESHOOTING.md` | Common issues |

## Credits

- Original DirettaRendererUPnP by Dominique COMET (cometdom)
- Diretta Host SDK by Yu Harada
- Low-latency optimizations by leeeanh
- MPD Diretta plugin by SwissMountainsBear
