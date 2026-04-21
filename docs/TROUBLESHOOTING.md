# Troubleshooting Guide - Diretta Host Daemon

Common problems and solutions.

## IPC Connection Issues

### Socket file does not exist

The daemon is not running. Start it:

```bash
sudo ./bin/DirettaRenderer
```

Check:
```bash
ls -la /tmp/diretta-renderer.sock
```

### Connection refused

The socket exists but connection fails. Check daemon status:

```bash
sudo ./bin/DirettaRenderer --verbose
```

Look for `[IPCServer] Listening on /tmp/...`.

### `control already held by another connection`

Another client is controlling playback. That client must:
```json
{"cmd":"release_control"}
```
Or disconnect.

## Target Issues

### No targets found

```bash
(printf '%s\n' '{"cmd":"discover_targets"}'; sleep 2) \
| sudo socat -T 5 - UNIX-CONNECT:/tmp/diretta-renderer.sock
```

Solutions:
- Ensure Diretta Target is running on the same network
- Specify network interface: `sudo ./bin/DirettaRenderer --interface eth0`
- Check firewall allows UDP 50001/50002
- If `socat` prints no JSON at all, make sure the command keeps the connection
  open with `sleep`; plain `printf ... | socat` can close before discovery
  returns.

### Target goes offline

```json
{"cmd":"select_target","target":"1"}
```
→ `{"ok":false,"error":"Failed to enable target #1"}`

Re-discover and select another:
```bash
(printf '%s\n' '{"cmd":"discover_targets"}'; sleep 2) \
| sudo socat -T 5 - UNIX-CONNECT:/tmp/diretta-renderer.sock

(printf '%s\n%s\n' \
  '{"cmd":"acquire_control"}' \
  '{"cmd":"select_target","target":"2"}'; sleep 3) \
| sudo socat -T 12 - UNIX-CONNECT:/tmp/diretta-renderer.sock
```

## Playback Issues

### `play` returns `ok:true` but no audio

1. Check transport: `{"cmd":"status"}` → `"transport":"playing"`
2. Check `bufferLevel` — if near 0, file may not exist or be unreadable
3. Verify NAS path is accessible from the machine running the daemon

### Audio glitches / dropouts

- Network congestion — ensure clean path to target
- Try larger MTU: `sudo ./bin/DirettaRenderer --mtu 9000`
- Lower process priority: `NICE_LEVEL=0` in config
- Check network errors: `ip -s link show`

### Format not supported

Try a standard file (FLAC 44.1kHz/16-bit) to verify the target works.
Check daemon logs: `sudo ./bin/DirettaRenderer --verbose`

## Build Issues

### SDK not found
```bash
make DIRETTA_SDK_PATH=/path/to/DirettaHostSDK_148
```

### FFmpeg version mismatch
Safe to ignore:
```bash
make FFMPEG_IGNORE_MISMATCH=1
```

## Diagnostic Commands

```bash
# Is daemon running?
ps aux | grep DirettaRenderer

# Daemon logs
./bin/DirettaRenderer --verbose

# Systemd logs
sudo journalctl -u diretta-renderer -f

# Network
ip addr show
ip route
```
