# Diretta Host Daemon v3.0.0

> **Based on [DirettaRendererUPnP](https://github.com/cometdom/DirettaRendererUPnP) by Dominique COMET (cometdom)** — v3.0 removes the UPnP/DLNA layer and replaces it with a Unix socket IPC interface for integration with external music players.

A native Linux daemon that streams high-resolution audio (DSD up to DSD1024, PCM up to 1536kHz) to any Diretta Target via Unix socket IPC.

**Not a UPnP renderer.** This daemon is designed for integration with external music players. Your player controls playback via JSON commands over a Unix domain socket.

## Architecture

```
┌──────────────────┐     Unix Socket IPC     ┌──────────────────────────┐
│  Your Music      │ ──────────────────────▶ │  Diretta Host Daemon      │
│  Player (Go)      │  JSON /tmp/xxx.sock    │  ┌──────────────────────┐ │
└──────────────────┘                          │  │  IPCServer          │ │
                                             │  │  (epoll, JSON)      │ │
                                             │  └──────────┬───────────┘ │
                                             │             │             │
                                             │  ┌──────────▼───────────┐ │
                                             │  │  AudioEngine         │ │
                                             │  │  (FFmpeg decode)     │ │
                                             │  └──────────┬───────────┘ │
                                             │             │             │
                                             │  ┌──────────▼───────────┐ │
                                             │  │  DirettaSync         │ │
                                             │  │  (SDK + ring buffer) │ │
                                             │  └──────────┬───────────┘ │
                                             └─────────────┼─────────────┘
                                                           │ UDP/Ethernet
                                                           ▼
                                             ┌──────────────────────────┐
                                             │  Diretta Target (DAC)   │
                                             └──────────────────────────┘
```

## Features

- **Bit-perfect streaming** to any discovered Diretta Target
- **DSD up to DSD1024**, PCM up to 1536kHz / 32-bit
- **Gapless playback** via AudioEngine internal preload
- **Runtime target selection** — discover targets, pick one at runtime
- **Lock-free audio hot path** with AVX2/NEON SIMD format conversion
- **Systemd service** or standalone binary

## Quick Start

### Build

```bash
# Extract SDK (if you have the tarball)
tar --use-compress-program=unzstd -xf DirettaHostSDK_148_13.tar.zst

# Build
make clean && make
```

### Run

```bash
# Start daemon (no target — use IPC to select at runtime)
sudo ./bin/DirettaRenderer

# Or connect to a specific target at startup (backward compat)
sudo ./bin/DirettaRenderer --target 1
```

### Test IPC

```bash
# Discover targets
echo '{"cmd":"discover_targets"}' | socat - UNIX-CONNECT:/tmp/diretta-renderer.sock

# Get control, select target, query status
echo '{"cmd":"acquire_control"}' | socat - UNIX-CONNECT:/tmp/diretta-renderer.sock
echo '{"cmd":"select_target","target":"1"}' | socat - UNIX-CONNECT:/tmp/diretta-renderer.sock
echo '{"cmd":"status"}' | socat - UNIX-CONNECT:/tmp/diretta-renderer.sock
```

## Build Variants

```bash
make ARCH_NAME=x64-linux-15v3    # x86-64 with AVX2 (most common)
make ARCH_NAME=x64-linux-15v4    # x86-64 with AVX-512
make ARCH_NAME=x64-linux-15zen4  # AMD Ryzen 7000+
make ARCH_NAME=aarch64-linux-15   # Raspberry Pi 4
make ARCH_NAME=aarch64-linux-15k16 # Raspberry Pi 5 (16KB pages)
make NOLOG=1                     # Production build (no SDK logging)
```

## Documentation

| Document | Description |
|----------|-------------|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | System architecture and design decisions |
| [docs/IPC_PROTOCOL.md](docs/IPC_PROTOCOL.md) | Complete IPC protocol reference |
| [docs/GO_INTEGRATION.md](docs/GO_INTEGRATION.md) | Go client integration guide |
| [docs/CONFIGURATION.md](docs/CONFIGURATION.md) | All configuration options |
| [docs/INSTALLATION.md](docs/INSTALLATION.md) | Installation and systemd setup |
| [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) | Common issues and solutions |

## Version History

- **v3.0.0** — Stripped UPnP/DLNA layer. Unix socket IPC interface. Runtime target selection via `select_target` command.
- **v2.1.x** — UPnP renderer with low-latency optimizations, AVX2 SIMD, lock-free ring buffer
