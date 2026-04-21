# Installation Guide - Diretta Host Daemon

## Requirements

- Linux (x86_64, aarch64, or riscv64)
- Diretta Host SDK v148 (download from https://www.diretta.link)
- FFmpeg 5.x (libavformat, libavcodec, libavutil, libswresample)
- GCC/G++ 7+, GNU make, pthread

## Dependencies

### Ubuntu / Debian

```bash
sudo apt install build-essential libavformat-dev libavcodec-dev libavutil-dev libswresample-dev pkg-config
```

### Fedora / RHEL

```bash
sudo dnf install gcc-c++ make ffmpeg-free-devel pkg-config
```

## Build

### 1. Obtain the Diretta SDK

Download `DirettaHostSDK_148_XX.tar.zst` from https://www.diretta.link and extract:

```bash
tar --use-compress-program=unzstd -xf DirettaHostSDK_148_XX.tar.zst
```

The directory name becomes your SDK path (e.g., `DirettaHostSDK_148/`).

### 2. Build

```bash
cd DirettaRendererUPnP

# Auto-detect architecture
make clean && make

# Or specify manually
make ARCH_NAME=x64-linux-15v3 DIRETTA_SDK_PATH=/path/to/DirettaHostSDK_148
```

The binary is placed at `bin/DirettaRenderer`.

### Architecture Variants

| Variant | CPU | Notes |
|---------|-----|-------|
| `x64-linux-15v2` | x86-64 baseline | No AVX2 |
| `x64-linux-15v3` | x86-64 + AVX2 | Most common |
| `x64-linux-15v4` | x86-64 + AVX-512 | Intel/AMD high-end |
| `x64-linux-15zen4` | AMD Zen 4 | Ryzen 7000/9000 |
| `aarch64-linux-15` | ARM64 (4KB pages) | Raspberry Pi 4 |
| `aarch64-linux-15k16` | ARM64 (16KB pages) | Raspberry Pi 5 |
| `riscv64-linux-15` | RISC-V 64-bit | |

### Production Build

```bash
make NOLOG=1
```

Disables SDK internal logging for lower CPU usage.

## Standalone Operation

```bash
# Start without a target (use IPC to select at runtime)
sudo ./bin/DirettaRenderer

# With custom socket path
sudo ./bin/DirettaRenderer --socket-path /tmp/my-renderer.sock

# Verbose logging
sudo ./bin/DirettaRenderer --verbose
```

**Requires `sudo`** because the Diretta SDK uses raw network sockets.

## Systemd Service

### Install

```bash
cd DirettaRendererUPnP

# Build first
make clean && make

# Install service
sudo ./systemd/install-systemd.sh
```

This copies the binary to `/opt/diretta-renderer/` and registers the systemd unit.

### Configure

Edit `/etc/default/diretta-renderer`:

```
TARGET=0                    # 0=none (IPC select), 1/2/3=auto-connect
SOCKET_PATH=/tmp/diretta-renderer.sock
INTERFACE=""                # or "eth0", "192.168.1.10"
CPU_AUDIO=""                # optional: Diretta SDK worker core, e.g. 2
CPU_OTHER=""                # optional: IPC/decode/logging core, e.g. 1
VERBOSE=""
NICE_LEVEL=-10
IO_SCHED_CLASS=realtime
IO_SCHED_PRIORITY=0
RT_PRIORITY=50
```

### Manage

```bash
# Reload config after editing
sudo systemctl daemon-reload
sudo systemctl restart diretta-renderer

# Start / stop / status
sudo systemctl start diretta-renderer
sudo systemctl stop diretta-renderer
sudo systemctl status diretta-renderer

# View logs
sudo journalctl -u diretta-renderer -f

# Disable auto-start
sudo systemctl disable diretta-renderer
```

### Uninstall

```bash
sudo ./systemd/uninstall-systemd.sh
```

## Network Requirements

- The machine must be on the same network as Diretta Targets
- Jumbo frames (MTU 9000+) recommended for hi-res audio (24-bit / 96kHz+)
- For multi-homed systems, set `INTERFACE` to the correct network interface

## Testing

```bash
# Start daemon without a startup target.
sudo ./bin/DirettaRenderer --socket-path /tmp/diretta-renderer.sock --verbose

# In another terminal, discover available targets:
(printf '%s\n' '{"cmd":"discover_targets"}'; sleep 2) \
| sudo socat -T 5 - UNIX-CONNECT:/tmp/diretta-renderer.sock

# Select target #1:
(printf '%s\n%s\n' \
  '{"cmd":"acquire_control"}' \
  '{"cmd":"select_target","target":"1"}'; sleep 3) \
| sudo socat -T 12 - UNIX-CONNECT:/tmp/diretta-renderer.sock

# Set URI:
(printf '%s\n%s\n' \
  '{"cmd":"acquire_control"}' \
  '{"cmd":"set_uri","path":"/path/to/test.wav"}'; sleep 2) \
| sudo socat -T 8 - UNIX-CONNECT:/tmp/diretta-renderer.sock

# Play:
(printf '%s\n%s\n' \
  '{"cmd":"acquire_control"}' \
  '{"cmd":"play"}'; sleep 5) \
| sudo socat -T 10 - UNIX-CONNECT:/tmp/diretta-renderer.sock

# Status:
(printf '%s\n' '{"cmd":"status"}'; sleep 1) \
| sudo socat -T 5 - UNIX-CONNECT:/tmp/diretta-renderer.sock

# Stop:
(printf '%s\n%s\n' \
  '{"cmd":"acquire_control"}' \
  '{"cmd":"stop"}'; sleep 2) \
| sudo socat -T 6 - UNIX-CONNECT:/tmp/diretta-renderer.sock
```
