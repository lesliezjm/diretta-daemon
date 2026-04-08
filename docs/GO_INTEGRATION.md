# Go Integration Guide

## Overview

Your Go music player connects to the daemon via Unix domain socket and sends JSON commands to control playback.

## Minimal Client

```go
package main

import (
    "bufio"
    "encoding/json"
    "fmt"
    "net"
    "os"
    "time"
)

type Response struct {
    Ok   bool    `json:"ok"`
    Error string `json:"error,omitempty"`
    Event string `json:"event,omitempty"`
    // status fields
    Transport  string  `json:"transport,omitempty"`
    Path       string  `json:"path,omitempty"`
    Position   float64 `json:"position,omitempty"`
    Duration   float64 `json:"duration,omitempty"`
    SampleRate uint32  `json:"sampleRate,omitempty"`
    BitDepth   uint32  `json:"bitDepth,omitempty"`
    Channels   uint32  `json:"channels,omitempty"`
    Format     string  `json:"format,omitempty"`
    DsdRate    int     `json:"dsdRate,omitempty"`
    BufferLevel float64 `json:"bufferLevel,omitempty"`
    // discover_targets fields
    Targets []Target `json:"targets,omitempty"`
    Count   int      `json:"count,omitempty"`
}

type Target struct {
    Index     int    `json:"index"`
    Name      string `json:"name"`
    Output    string `json:"output"`
    PortIn    uint16 `json:"portIn"`
    PortOut   uint16 `json:"portOut"`
    Multiport bool   `json:"multiport"`
    Config    string `json:"config"`
    Version   uint16 `json:"version"`
    ProductId string `json:"productId"`
}

type Client struct {
    conn    net.Conn
    dec     *json.Decoder
    control bool
}

func NewClient(sockPath string) (*Client, error) {
    conn, err := net.Dial("unix", sockPath)
    if err != nil {
        return nil, err
    }
    return &Client{conn: conn, dec: json.NewDecoder(conn)}, nil
}

func (c *Client) Send(cmd map[string]interface{}) (*Response, error) {
    if err := json.NewEncoder(c.conn).Encode(cmd); err != nil {
        return nil, err
    }
    var resp Response
    if err := c.dec.Decode(&resp); err != nil {
        return nil, err
    }
    return &resp, nil
}

func (c *Client) AcquireControl() error {
    resp, err := c.Send(map[string]interface{}{"cmd": "acquire_control"})
    if err != nil {
        return err
    }
    if !resp.Ok {
        return fmt.Errorf("acquire control failed: %s", resp.Error)
    }
    c.control = true
    return nil
}

func (c *Client) DiscoverTargets() ([]Target, error) {
    resp, err := c.Send(map[string]interface{}{"cmd": "discover_targets"})
    if err != nil {
        return nil, err
    }
    if !resp.Ok {
        return nil, fmt.Errorf("discover failed: %s", resp.Error)
    }
    return resp.Targets, nil
}

func (c *Client) SelectTarget(index int) error {
    resp, err := c.Send(map[string]interface{}{
        "cmd":    "select_target",
        "target": fmt.Sprintf("%d", index),
    })
    if err != nil {
        return err
    }
    if !resp.Ok {
        return fmt.Errorf("select target failed: %s", resp.Error)
    }
    return nil
}

func (c *Client) Play(path string) error {
    resp, err := c.Send(map[string]interface{}{
        "cmd":  "play",
        "path": path,
    })
    if err != nil {
        return err
    }
    if !resp.Ok {
        return fmt.Errorf("play failed: %s", resp.Error)
    }
    return nil
}

func (c *Client) Status() (*Response, error) {
    return c.Send(map[string]interface{}{"cmd": "status"})
}

func (c *Client) Close() error {
    return c.conn.Close()
}

// ListenEvents reads push notifications in a goroutine.
// Call this after acquiring control and before starting playback.
func (c *Client) ListenEvents(handler func(Response)) {
    go func() {
        for {
            var resp Response
            if err := c.dec.Decode(&resp); err != nil {
                return
            }
            if resp.Event != "" {
                handler(resp)
            }
        }
    }()
}

func main() {
    // Connect
    client, err := NewClient("/tmp/diretta-renderer.sock")
    if err != nil {
        fmt.Fprintf(os.Stderr, "connect failed: %v\n", err)
        os.Exit(1)
    }
    defer client.Close()

    // Acquire control
    if err := client.AcquireControl(); err != nil {
        fmt.Fprintf(os.Stderr, "%v\n", err)
        os.Exit(1)
    }
    fmt.Println("Control acquired")

    // Discover targets
    targets, err := client.DiscoverTargets()
    if err != nil {
        fmt.Fprintf(os.Stderr, "%v\n", err)
        os.Exit(1)
    }
    if len(targets) == 0 {
        fmt.Println("No targets found")
        os.Exit(1)
    }
    fmt.Printf("Found %d targets:\n", len(targets))
    for _, t := range targets {
        fmt.Printf("  [%d] %s (%s)\n", t.Index, t.Name, t.Output)
    }

    // Select first target
    if err := client.SelectTarget(1); err != nil {
        fmt.Fprintf(os.Stderr, "%v\n", err)
        os.Exit(1)
    }
    fmt.Println("Target connected")

    // Start event listener
    client.ListenEvents(func(resp Response) {
        switch resp.Event {
        case "state_change":
            fmt.Printf("State: %s\n", resp.Transport)
        case "track_change":
            fmt.Printf("Track: %s (%s/%dbit/%dch)\n",
                resp.Path, resp.Format, resp.BitDepth, resp.Channels)
        case "position":
            fmt.Printf("Position: %.1f / %.1f\n", resp.Position, resp.Duration)
        }
    })

    // Play
    if err := client.Play("/nas/music/test.flac"); err != nil {
        fmt.Fprintf(os.Stderr, "%v\n", err)
        os.Exit(1)
    }

    // Keep running
    select {}
}
```

## Key Patterns

### 1. Control Acquisition

Always acquire control before sending playback commands:

```go
client.AcquireControl()
```

If another connection holds control, this returns an error. The daemon can be shared by waiting for the current holder to `release_control`.

### 2. Target Selection

`discover_targets` returns a list of available targets. The `index` field is 1-based and matches the order returned by the discovery scan:

```go
targets, _ := client.DiscoverTargets()
for _, t := range targets {
    fmt.Printf("[%d] %s\n", t.Index, t.Name)
}
client.SelectTarget(1) // 1-based
```

### 3. Event Loop

Call `ListenEvents()` before starting playback. Events arrive on the same connection as command responses — you must read from the connection in a separate goroutine:

```go
client.ListenEvents(func(resp Response) {
    switch resp.Event {
    case "state_change": ...
    case "track_change": ...
    case "position": ...
    }
})
```

### 4. Gapless Playback

Send the next `play(path)` command while the current track is still playing. The AudioEngine handles preloading internally. There is no explicit gapless API.

### 5. Seek

```go
client.Send(map[string]interface{}{
    "cmd":      "seek",
    "position": 120.5, // seconds
})
```

### 6. Graceful Shutdown

```go
client.Send(map[string]interface{}{"cmd": "stop"})
client.Send(map[string]interface{}{"cmd": "release_control"})
client.Close()
```

## Complete Workflow

```
1. net.Dial("unix", "/tmp/diretta-renderer.sock")
2. acquire_control
3. discover_targets → show targets to user
4. select_target → connects and warms up
5. ListenEvents() → start goroutine
6. play(path) → starts playback
   ← state_change: playing
   ← track_change: format info
   ← position: every 1s
7. pause() / resume() / stop() / seek()
8. select_target(2) → switch to different target
9. release_control + close
```
