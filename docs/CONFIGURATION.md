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
| `--pcm-output-mode <mode>` | PCM sink bit-depth diagnostic mode (see below) | `auto` |
| `--mtu <bytes>` | MTU override (auto-detect default) | auto |
| `--rt-priority <1-99>` | SCHED_FIFO real-time priority for audio thread | `50` |
| `--cpu-audio <core>` | Pin Diretta SDK occupied worker thread to a CPU core | disabled |
| `--cpu-other <core>` | Pin main, IPC, decode, logging, and position threads to a CPU core | disabled |

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

### PCM Output Modes

These modes are for diagnosing sink bit-depth negotiation. Force modes do not fall back.

| Mode | Description |
|------|-------------|
| `auto` | Current default negotiation |
| `force16` | Require 16-bit PCM sink support |
| `force24` | Require 24-bit PCM sink support |
| `force32` | Require 32-bit PCM sink support |
| `prefer32` | Try 32-bit first, then fall back to default order |

## Systemd Configuration

The service reads `/etc/default/diretta-renderer` (see `systemd/diretta-renderer.conf`):

```
# Target: 0=none (IPC), 1/2/3...=auto-connect at startup
TARGET=0

# IPC socket path
SOCKET_PATH="/tmp/diretta-renderer.sock"

# Network interface
INTERFACE=""

# CPU affinity
# Empty = disabled. Use different physical cores when possible.
CPU_AUDIO=""
CPU_OTHER=""

# Log level
VERBOSE=""

# PCM output mode
PCM_OUTPUT_MODE=auto

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

`NETWORK_INTERFACE` and `MTU_OVERRIDE` are still accepted as aliases by the
systemd wrapper for compatibility with older config files.

## IPC Commands

All IPC commands are documented in [IPC_PROTOCOL.md](IPC_PROTOCOL.md).

Key commands for runtime configuration:
- `discover_targets` — scan network for targets
- `select_target` — connect to a specific target at runtime
- `set_uri` — load the current track without starting playback
- `play` — start playback of the current URI, or resume from pause
- `status` — query playback state

Manual playback smoke test:

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
