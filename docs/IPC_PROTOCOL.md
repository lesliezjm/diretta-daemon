# IPC Protocol Reference

JSON over Unix domain socket. Line-delimited messages (one JSON object per line, ending with `\n`).

## Connection Model

- **Multiple connections** can connect simultaneously
- **All connections** can send: `discover_targets`, `status`, `acquire_control`, `release_control`
- **Only one connection** at a time holds control (can send play/pause/stop/seek/select_target)
- **All connections** receive push notifications (state change, track change, position)

## Socket Path

Default: `/tmp/diretta-renderer.sock` (configurable via `--socket-path`)

## Commands

All commands use the same request format. Responses vary.

---

### `discover_targets`

Discover available Diretta Targets on the network.

**Request:**
```json
{"cmd":"discover_targets"}
```

**Response:**
```json
{
  "ok": true,
  "targets": [
    {
      "index": 1,
      "name": "GentooPlayer",
      "output": "I2S",
      "portIn": 50001,
      "portOut": 50002,
      "multiport": false,
      "config": "http://192.168.1.100:8080",
      "version": 148,
      "productId": "0x93"
    },
    {
      "index": 2,
      "name": "MemoryPlay",
      "output": "USB",
      "portIn": 50001,
      "portOut": 50002,
      "multiport": true,
      "config": "",
      "version": 147,
      "productId": "0x10"
    }
  ],
  "count": 2
}
```

No targets found:
```json
{"ok":true,"targets":[],"count":0}
```

---

### `status`

Query current playback state. Available to all connections.

**Request:**
```json
{"cmd":"status"}
```

**Response:**
```json
{
  "ok": true,
  "status": {
    "transport": "playing",
    "path": "/nas/music/song.flac",
    "position": 42.5,
    "duration": 240.0,
    "sampleRate": 96000,
    "bitDepth": 24,
    "channels": 2,
    "format": "PCM",
    "dsdRate": 0,
    "bufferLevel": 0.73,
    "trackNumber": 1
  }
}
```

Transport states: `playing`, `paused`, `stopped`

---

### `acquire_control`

Request control of playback. Only one connection can hold control at a time.

**Request:**
```json
{"cmd":"acquire_control"}
```

**Response (success):**
```json
{"ok":true}
```

**Response (already held):**
```json
{"ok":false,"error":"control already held by another connection"}
```

---

### `release_control`

Release control of playback. Other connections can then acquire it.

**Request:**
```json
{"cmd":"release_control"}
```

**Response:**
```json
{"ok":true}
```

---

### `play`

Start playback. Requires control.

**Start a new track:**
```json
{"cmd":"play","path":"/nas/music/song.flac"}
```

**Resume from pause (no path):**
```json
{"cmd":"play"}
```

**Response:**
```json
{"ok":true}
```

---

### `pause`

Pause playback. Requires control.

**Request:**
```json
{"cmd":"pause"}
```

**Response:**
```json
{"ok":true}
```

---

### `stop`

Stop playback. Requires control.

**Request:**
```json
{"cmd":"stop"}
```

**Response:**
```json
{"ok":true}
```

---

### `seek`

Seek to a position in seconds. Requires control.

**Request:**
```json
{"cmd":"seek","position":120.5}
```

**Response:**
```json
{"ok":true}
```

---

### `select_target`

Connect to a specific Diretta Target. Requires control.

Disconnects from the current target (if any), scans the network, and connects to the selected target with warmup.

**Request:**
```json
{"cmd":"select_target","target":"1"}
```

`target` is 1-based (1 = first discovered, 2 = second, etc.)

**Response:**
```json
{"ok":true}
```

If target is not found (e.g., target went offline):
```json
{"ok":false,"error":"Failed to enable target #1"}
```

---

### `shutdown`

Shutdown the daemon. Requires control.

**Request:**
```json
{"cmd":"shutdown"}
```

**Response:**
```json
{"ok":true}
```

The daemon then exits.

---

## Push Notifications

These are sent automatically by the daemon to **all connected clients** (no request needed).

### `state_change`

```json
{"event":"state_change","transport":"playing"}
{"event":"state_change","transport":"paused"}
{"event":"state_change","transport":"stopped"}
```

### `track_change`

```json
{"event":"track_change","path":"/nas/music/new.flac","sampleRate":44100,"bitDepth":16,"channels":2,"format":"PCM","duration":200.0}
```

### `position`

Pushed every 1 second while playing.

```json
{"event":"position","position":42.5,"duration":240.0}
```

---

## Error Responses

```json
{"ok":false,"error":"not a control connection (send acquire_control first)"}
{"ok":false,"error":"missing 'cmd' field"}
{"ok":false,"error":"missing 'target' field (1-based index)"}
{"ok":false,"error":"unknown command: foobar"}
```

## Interaction Example

```
Client → {"cmd":"accover_control"}
Server ← {"ok":true}

Client → {"cmd":"discover_targets"}
Server ← {"ok":true,"targets":[...],"count":2}

Client → {"cmd":"select_target","target":"1"}
Server ← {"ok":true}

Client → {"cmd":"play","path":"/nas/song.flac"}
Server ← {"ok":true}

Server ← {"event":"state_change","transport":"playing"}
Server ← {"event":"track_change","path":"/nas/song.flac","sampleRate":96000,...}
Server ← {"event":"position","position":1.0,"duration":180.0}
Server ← {"event":"position","position":2.0,"duration":180.0}
...
Server ← {"event":"state_change","transport":"stopped"}
```
