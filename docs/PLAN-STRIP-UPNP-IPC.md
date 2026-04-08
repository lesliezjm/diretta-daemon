# Plan: Strip UPnP, Replace with Unix Socket IPC

## Context

将 DirettaRendererUPnP 从 UPnP/DLNA 渲染器改造为通用的 **Diretta Host Daemon**，通过 Unix domain socket 提供 IPC 接口，供外部音乐播放器（Go 语言后端）集成。

音乐播放器已经完成了浏览专辑、获取播放文件 NAS 路径的逻辑，只需要通过 IPC 控制这个 daemon 进行播放。

**核心原则：绝不修改 Diretta SDK 集成代码（DirettaSync、DirettaRingBuffer）。音频热路径、格式协商、流控 — 全部原样保留。**

---

## 当前架构分析

```
UPnP Control Point → UPnPDevice (libupnp) → DirettaRenderer (编排器) → AudioEngine (FFmpeg解码) → DirettaSync (SDK环形缓冲) → Diretta Target DAC
```

### 数据流详解

1. **UPnP Control Point** 发送 SetAVTransportURI + Play（SOAP）
2. **UPnPDevice** 接收 SOAP 请求，通过 `std::function` 回调通知 DirettaRenderer
3. **DirettaRenderer** 的回调函数调用 AudioEngine 设置 URI、启动播放
4. **AudioThread** 循环调用 `AudioEngine::process()` 解码音频
5. **AudioEngine** 解码后调用 `m_audioCallback(buffer, samples, rate, bits, channels)`
6. **回调 lambda**（DirettaRenderer.cpp:230-468）构建 AudioFormat，调用 `DirettaSync::open()` 配置格式，调用 `DirettaSync::sendAudio()` 推送到环形缓冲
7. **DirettaSync** 的 SDK worker 线程通过 `getNewStream()` 回调从环形缓冲拉取数据发送到网络

### 文件分类

| 分类 | 文件 | 说明 |
|------|------|------|
| **纯 UPnP** | `UPnPDevice.hpp/cpp`, `ProtocolInfoBuilder.h` | libupnp，无 Diretta/音频知识 |
| **纯音频** | `AudioEngine.h/cpp` | FFmpeg 解码，无 UPnP/Diretta 知识 |
| **纯 Diretta** | `DirettaSync.h/cpp`, `DirettaRingBuffer.h` | SDK 集成，无 UPnP/FFmpeg 知识 |
| **编排器** | `DirettaRenderer.h/cpp` | 连接 UPnP→Audio→Diretta，包含所有胶水逻辑 |
| **入口** | `main.cpp` | CLI 解析，创建 DirettaRenderer |
| **共享** | `LogLevel.h`, `TimestampedLogger.h`, `memcpyfast*.h` | 日志、SIMD 优化 |

### 关键发现

- `DirettaRenderer.cpp` 的 **音频回调 lambda（230-468行）** 是核心胶水逻辑：处理格式变化、DSD 转换、流控。这部分代码 **必须保留**
- `audioThreadFunc()` 包含缓冲区水位控制和流控逻辑，**必须保留**
- UPnP 回调（onSetURI, onPlay, onStop, onSeek 等）中的业务逻辑（自动停止、DAC 稳定、空闲释放）**保留**，仅改变触发源
- `UPnPDevice` 完全独立，可以安全删除

---

## IPC 协议设计

### 传输层

- **Unix domain socket**: `/tmp/diretta-renderer.sock`（可通过 `--socket-path` 配置）
- **协议格式**: 换行分隔的 JSON（每行一个 JSON 对象）
- **连接模型**: 多个只读状态连接 + 单个控制连接

### 连接角色

| 角色 | 数量 | 权限 |
|------|------|------|
| Status（状态） | 多个 | 只读：接收推送通知，可查询状态 |
| Control（控制） | 单个 | 读写：发送播放控制命令 |

状态连接通过发送 `acquire_control` 提升为控制连接。控制连接断开时自动释放。

### 命令列表

| 命令 | 请求 JSON | 响应 | 说明 |
|------|-----------|------|------|
| play | `{"cmd":"play","path":"/music/song.flac"}` | `{"ok":true}` | 播放指定路径 |
| resume | `{"cmd":"play"}` | `{"ok":true}` | 从暂停恢复 |
| pause | `{"cmd":"pause"}` | `{"ok":true}` | 暂停播放 |
| stop | `{"cmd":"stop"}` | `{"ok":true}` | 停止播放 |
| seek | `{"cmd":"seek","position":120.5}` | `{"ok":true}` | 跳转到指定秒数 |
| status | `{"cmd":"status"}` | `{"ok":true,"status":{...}}` | 查询完整状态 |
| acquire_control | `{"cmd":"acquire_control"}` | `{"ok":true}` 或错误 | 提升为控制连接 |
| release_control | `{"cmd":"release_control"}` | `{"ok":true}` | 释放控制权 |
| shutdown | `{"cmd":"shutdown"}` | `{"ok":true}` | 优雅关闭 daemon |

### 状态快照（status 响应和推送通知）

```json
{
  "ok": true,
  "status": {
    "transport": "playing",
    "path": "/music/song.flac",
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

### 推送通知（广播到所有连接的客户端）

```json
{"event":"state_change","transport":"stopped"}
{"event":"track_change","path":"/music/new.flac","sampleRate":44100,"bitDepth":16,"channels":2,"format":"PCM","duration":200.0}
{"event":"position","position":42.5,"duration":240.0}
```

### Go 端集成示例

```go
import (
    "encoding/json"
    "net"
    "bufio"
)

conn, _ := net.Dial("unix", "/tmp/diretta-renderer.sock")

// 获取控制权
json.NewEncoder(conn).Encode(map[string]string{"cmd": "acquire_control"})

// 播放
json.NewEncoder(conn).Encode(map[string]string{
    "cmd":  "play",
    "path": "/nas/music/song.flac",
})

// 读取响应和通知
scanner := bufio.NewScanner(conn)
for scanner.Scan() {
    var msg map[string]interface{}
    json.Unmarshal(scanner.Bytes(), &msg)
    if _, hasEvent := msg["event"]; hasEvent {
        // 处理推送通知
    } else {
        // 处理命令响应
    }
}
```

---

## 文件变更清单

### 新建（2 个文件）

| 文件 | 用途 |
|------|------|
| `src/IPCServer.h` | Unix socket 服务器类声明，JSON 协议定义，连接管理 |
| `src/IPCServer.cpp` | epoll 事件循环实现，JSON 解析/序列化，控制/状态连接模型 |

### 修改（5 个文件）

| 文件 | 变更内容 |
|------|----------|
| `src/DirettaRenderer.h` | 替换 `UPnPDevice` → `IPCServer`；删除 `m_upnpThread`/`upnpThreadFunc()`；删除 UPnP 配置字段（port, uuid）；添加 `socketPath`；添加 `buildStatusSnapshot()` |
| `src/DirettaRenderer.cpp` | 替换 UPnP 回调绑定为 IPC 命令处理。**保留音频回调 lambda（230-468行）完全不变** — 仅将通知目标从 `m_upnp->` 改为 `m_ipc->` |
| `src/main.cpp` | 删除 `g_minimalUPnP`、UPnP CLI 参数（`--name`, `--port`, `--uuid`, `--no-gapless`, `--minimal-upnp`）；添加 `--socket-path`；更新帮助文本 |
| `src/DirettaSync.h` | 删除 `extern bool g_minimalUPnP;`（第108行）。**其他一行不动** |
| `Makefile` | 从 SOURCES 删除 `UPnPDevice.cpp`，添加 `IPCServer.cpp`；从 LIBS 删除 `-lupnp -lixml`；删除 UPnP include 检测块；重命名二进制为 `DirettaRenderer` |

### 删除（3 个文件）

| 文件 | 原因 |
|------|------|
| `src/UPnPDevice.hpp` | 纯 UPnP — 整个类删除 |
| `src/UPnPDevice.cpp` | 纯 UPnP — 整个类删除 |
| `src/ProtocolInfoBuilder.h` | 纯 UPnP — 协议信息生成删除 |

### 绝不修改（禁止触碰）

| 文件 | 原因 |
|------|------|
| `src/DirettaSync.cpp` | Diretta SDK 集成 — 热路径 |
| `src/DirettaRingBuffer.h` | 无锁 SPSC 环形缓冲 — 热路径 |
| `src/AudioEngine.h`, `src/AudioEngine.cpp` | FFmpeg 解码器 — 独立模块，无 UPnP 知识 |
| `src/fastmemcpy-avx.c`, `src/memcpyfast*.h`, `src/FastMemcpy_*.h` | SIMD 优化 |
| `src/LogLevel.h`, `src/TimestampedLogger.h` | 共享基础设施 |
| `src/DirettaOutput.h`, `src/DirettaOutput.cpp` | 遗留代码，未编译 |
| `src/test_audio_memory.cpp`, `src/AudioMemoryTest.h` | 测试 |
| `src/sync/` 目录 | 重复/备份，未编译 |
| 所有 SDK 头文件和库 | 专有，已可工作 |

---

## DirettaRenderer.cpp 重构细节

### start() 方法

| 行号范围 | 内容 | 操作 |
|----------|------|------|
| 129-202 | DirettaSync 创建、配置、enable、预连接 | **原样保留** |
| 204-224 | UPnPDevice 创建、位置回调 | **替换**为 IPCServer 创建 + 回调绑定 |
| 230-386 | 音频回调 lambda（格式变化、DSD 转换、流控） | **原样保留** — 核心胶水 |
| 392-425 | 曲目变化回调 | 保留逻辑，`m_upnp->notifyGaplessTransition()` → `m_ipc->notifyTrackChange()` |
| 427-468 | 曲目结束回调 | 保留逻辑，`m_upnp->notifyStateChange()` → `m_ipc->notifyStateChange()` |
| 474-632 | UPnP 回调（onSetURI, onPlay, onStop, onSeek 等） | **替换**为 IPC 命令处理。保留业务逻辑（自动停止、DAC 稳定、空闲释放） |
| 634-676 | UPnP 服务器启动重试循环 | **删除** |
| 680-688 | 线程创建 | 删除 `m_upnpThread`，保留 `m_audioThread` |

### stop() 方法

- `m_upnp->stop()` → `m_ipc->stop()`
- 删除 `m_upnpThread.join()`
- 保留 `m_audioThread.join()`

### audioThreadFunc()

**原样保留** — 缓冲区水位控制和流控是性能关键代码

### positionThreadFunc()

简化为每秒调用 `m_ipc->notifyPosition(position, duration)`。删除 UPnP epoch/gapless 竞态逻辑。

---

## 实施阶段

### Phase 1: 创建 IPCServer（新文件，零现有代码变更）

1. 编写 `src/IPCServer.h` — 类声明
2. 编写 `src/IPCServer.cpp` — 完整实现：
   - `socket()` → `bind()` → `listen()` → epoll 事件循环
   - 非阻塞 I/O，连接跟踪，行缓冲 JSON
   - 手工 JSON 构建/解析（无外部依赖 — 协议最多 10 个字段）
   - `broadcast()` 使用 `try_lock`（音频线程中的通知尽力而为）
   - `unlink()` 在 `bind()` 之前清理残留 socket 文件

### Phase 2: 重构 DirettaRenderer

3. **DirettaRenderer.h**: `#include "UPnPDevice.hpp"` → `#include "IPCServer.h"`；`m_upnp` → `m_ipc`；更新 Config 结构体
4. **DirettaRenderer.cpp — start()**: 替换 UPnP 创建为 IPC 创建，保留音频回调和 DirettaSync 初始化
5. **DirettaRenderer.cpp — stop()**: 替换 UPnP 停止为 IPC 停止
6. **DirettaRenderer.cpp — audioThreadFunc()**: 原样保留
7. **DirettaRenderer.cpp — positionThreadFunc()**: 简化为 IPC 位置推送

### Phase 3: 更新 main.cpp 和 DirettaSync.h

8. **main.cpp**: 删除 `g_minimalUPnP`，替换 UPnP CLI 参数，添加 `--socket-path`
9. **DirettaSync.h**（`src/` 和 `src/sync/` 两份）: 删除 `extern bool g_minimalUPnP;`

### Phase 4: Makefile + 清理

10. **Makefile**: 更新 SOURCES、LIBS、二进制名称
11. 删除 `src/UPnPDevice.hpp`、`src/UPnPDevice.cpp`、`src/ProtocolInfoBuilder.h`
12. `make clean && make` — 验证编译成功

---

## 验证步骤

1. **编译**: `make clean && make` — 无错误，不依赖 libupnp
2. **列出目标**: `sudo ./bin/DirettaRenderer --list-targets` — Diretta 发现正常
3. **Socket 创建**: `sudo ./bin/DirettaRenderer --target 1` — 验证 `/tmp/diretta-renderer.sock` 存在
4. **IPC 测试**:
   ```bash
   socat - UNIX-CONNECT:/tmp/diretta-renderer.sock
   # 输入：
   {"cmd":"acquire_control"}
   {"cmd":"status"}
   {"cmd":"play","path":"/path/to/test.flac"}
   {"cmd":"stop"}
   ```
5. **Go 集成测试**: 编写最小 Go 客户端，连接、获取控制、发送播放、读取状态

---

## Go 播放器集成建议

### 基本集成流程

```go
// 1. 连接 socket
conn, err := net.Dial("unix", "/tmp/diretta-renderer.sock")

// 2. 获取控制权
sendCmd(conn, map[string]string{"cmd": "acquire_control"})

// 3. 播放音乐（从数据库/NAS 获取的路径）
sendCmd(conn, map[string]string{"cmd": "play", "path": "/nas/music/album/01-track.flac"})

// 4. 读取循环：区分响应和通知
scanner := bufio.NewScanner(conn)
for scanner.Scan() {
    var msg map[string]interface{}
    json.Unmarshal(scanner.Bytes(), &msg)
    if _, isEvent := msg["event"]; isEvent {
        // 推送通知：更新 UI
    } else {
        // 命令响应：检查 ok 字段
    }
}

// 5. 播放列表中的下一首（无缝）
// 在当前曲目播放期间发送下一个 play 命令
// AudioEngine 内部处理预加载
sendCmd(conn, map[string]string{"cmd": "play", "path": "/nas/music/album/02-track.flac"})

// 6. 优雅关闭
sendCmd(conn, map[string]string{"cmd": "stop"})
sendCmd(conn, map[string]string{"cmd": "release_control"})
conn.Close()
```

### 集成要点

| 要点 | 说明 |
|------|------|
| 连接方式 | `net.Dial("unix", "/tmp/diretta-renderer.sock")` |
| 先获取控制 | 发送 `acquire_control`，再发送 play/pause/stop/seek |
| 读取循环 | 按行扫描，通过 `event` 字段区分通知和响应 |
| 状态轮询 | 发送 `status` 命令，或依赖推送的 position 事件（每秒） |
| 无缝播放 | 在当前曲目播放的最后几秒发送下一个 `play(path)`，AudioEngine 内部处理预加载 |
| 优雅关闭 | 先 `stop`，再 `release_control`，最后断开连接 |
| 断线重连 | daemon 保持运行，直接重连即可 |
| 多客户端 | 多个只读连接可同时监听状态变化 |

### daemon 端部署建议

```bash
# 命令行启动
sudo ./bin/DirettaRenderer --target 1 --socket-path /tmp/diretta-renderer.sock --verbose

# systemd 服务启动
sudo systemctl start diretta-renderer
# socket 路径配置在 /etc/default/diretta-renderer 中
```
