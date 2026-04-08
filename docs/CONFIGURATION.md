# Configuration Guide - Diretta Host Daemon

## Command-Line Options

### Basic

| Option | Description |
|--------|-------------|
| `--socket-path, -s <path>` | IPC socket path (default: `/tmp/diretta-renderer.sock`) |
| `--target, -t <index>` | Select Diretta target by index (1-based). `0` or omit = no target, use IPC `select_target` |
| `--interface <name>` | Network interface name or IP address (e.g., `eth0`, `192.168.1.10`). Default: auto-detect |
| `--list-targets, -l` | Scan network, list available targets, exit |
| `--verbose, -v` | Enable debug logging |
| `--quiet, -q` | Quiet mode — errors and warnings only |
| `--version, -V` | Show version |

### Advanced (Diretta SDK)

| Option | Description | Default |
|--------|-------------|---------|
| `--thread-mode <mode>` | SDK thread mode bitmask (see below) | `1` (CRITICAL) |
| `--cycle-time <us>` | Max transfer cycle time (333–10000 µs) | auto |
| `--cycle-min-time <us>` | Min cycle time (random mode only) | `333` |
| `--info-cycle <us>` | Info packet cycle (µs) | `100000` |
| `--transfer-mode <mode>` | Transfer mode (see below) | `auto` |
| `--target-profile-limit <us>` | 0=SelfProfile (stable), >0=TargetProfile (experimental) | `0` |
| `--mtu <bytes>` | MTU override (auto-detect default) | auto |
| `--rt-priority <1-99>` | SCHED_FIFO real-time priority for audio thread | `50` |

### Thread Mode Bitmask

Add values together to combine:

| Value | Flag | Description |
|-------|------|-------------|
| `1` | CRITICAL | Real-time priority sending thread |
| `2` | NOSHORTSLEEP | Busy loop for short waits |
| `4` | NOSLEEP4CORE | Disable busy loop if <4 cores |
| `8` | SOCKETNOBLOCK | Non-blocking socket |
| `16` | OCCUPIED | Pin thread to CPU |
| `32` | FEEDBACK32 | Moving average feedback |
| `2048` | NOSLEEPFORCE | Force busy loop |
| `8192` | NOJUMBOFRAME | Disable jumbo frames |

Examples: `1` = default, `17` = CRITICAL + OCCUPIED, `33` = CRITICAL + FEEDBACK32

### Transfer Modes

| Mode | Description |
|------|-------------|
| `auto` | Automatic (VarMax for PCM Hi-Res, VarAuto for DSD/low-bitrate) |
| `varmax` | Flex cycle, maximum packet size |
| `varauto` | Flex cycle, auto-tuned |
| `fixauto` | Fixed cycle, auto-tuned |
| `random` | Random cycle (also set `--cycle-min-time`) |

## Systemd Configuration

The service reads `/etc/default/diretta-renderer` (see `systemd/diretta-renderer.conf`):

```
# Target: 0=none (IPC), 1/2/3...=auto-connect at startup
TARGET=0

# IPC socket path
SOCKET_PATH="/tmp/diretta-renderer.sock"

# Network interface
INTERFACE=""

# Log level
VERBOSE=""

# Process priority
NICE_LEVEL=-10
IO_SCHED_CLASS=realtime
IO_SCHED_PRIORITY=0
RT_PRIORITY=50
```

## Environment Variables

The following old UPnP variable names are **no longer used** and ignored:

- `PORT` — was UPnP HTTP port (removed)
- `NAME` — was renderer friendly name (removed)
- `GAPLESS` — was gapless toggle (always enabled)
- `MINIMAL_UPNP` — was UPnP minimal mode (removed)

## IPC Commands

All IPC commands are documented in [IPC_PROTOCOL.md](IPC_PROTOCOL.md).

Key commands for runtime configuration:
- `discover_targets` — scan network for targets
- `select_target` — connect to a specific target at runtime
- `status` — query playback state
