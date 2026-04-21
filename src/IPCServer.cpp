/**
 * @file IPCServer.cpp
 * @brief Unix domain socket IPC server implementation
 *
 * epoll-based event loop with line-buffered JSON protocol.
 * Supports multiple status (read-only) connections and a single control connection.
 */

#include "IPCServer.h"
#include "LogLevel.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <cstdio>
#include <algorithm>

//=============================================================================
// Constants
//=============================================================================

static constexpr int MAX_EVENTS = 64;
static constexpr int READ_BUF_SIZE = 4096;
static constexpr char CMD_ACQUIRE_CONTROL[] = "acquire_control";
static constexpr char CMD_RELEASE_CONTROL[] = "release_control";
static constexpr char CMD_PLAY[] = "play";
static constexpr char CMD_SET_URI[] = "set_uri";
static constexpr char CMD_QUEUE_NEXT[] = "queue_next";
static constexpr char CMD_PLAY_NOW[] = "play_now";
static constexpr char CMD_PAUSE[] = "pause";
static constexpr char CMD_STOP[] = "stop";
static constexpr char CMD_SEEK[] = "seek";
static constexpr char CMD_STATUS[] = "status";
static constexpr char CMD_SHUTDOWN[] = "shutdown";
static constexpr char CMD_DISCOVER_TARGETS[] = "discover_targets";
static constexpr char CMD_SELECT_TARGET[] = "select_target";

//=============================================================================
// Helpers
//=============================================================================

static void setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

//=============================================================================
// Constructor / Destructor
//=============================================================================

IPCServer::IPCServer() = default;

IPCServer::~IPCServer() {
    stop();
}

//=============================================================================
// Lifecycle
//=============================================================================

bool IPCServer::start(const std::string& socketPath, StatusProvider provider) {
    if (m_running) {
        LOG_WARN("[IPCServer] Already running");
        return false;
    }

    m_socketPath = socketPath;
    m_statusProvider = std::move(provider);

    // Clean up any stale socket file
    unlink(m_socketPath.c_str());

    // Create listening socket
    m_listenFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_listenFd < 0) {
        LOG_ERROR("[IPCServer] socket() failed: " << strerror(errno));
        return false;
    }

    // Allow address reuse
    int opt = 1;
    setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, m_socketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(m_listenFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("[IPCServer] bind() failed: " << strerror(errno));
        close(m_listenFd);
        m_listenFd = -1;
        return false;
    }

    // Listen
    if (listen(m_listenFd, 8) < 0) {
        LOG_ERROR("[IPCServer] listen() failed: " << strerror(errno));
        close(m_listenFd);
        m_listenFd = -1;
        unlink(m_socketPath.c_str());
        return false;
    }

    setNonBlocking(m_listenFd);

    // Create epoll instance
    m_epollFd = epoll_create1(0);
    if (m_epollFd < 0) {
        LOG_ERROR("[IPCServer] epoll_create1() failed: " << strerror(errno));
        close(m_listenFd);
        m_listenFd = -1;
        unlink(m_socketPath.c_str());
        return false;
    }

    // Add listen socket to epoll
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = m_listenFd;
    if (epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_listenFd, &ev) < 0) {
        LOG_ERROR("[IPCServer] epoll_ctl() failed: " << strerror(errno));
        close(m_epollFd);
        m_epollFd = -1;
        close(m_listenFd);
        m_listenFd = -1;
        unlink(m_socketPath.c_str());
        return false;
    }

    m_running = true;
    m_thread = std::thread(&IPCServer::eventLoop, this);

    std::cout << "[IPCServer] Listening on " << m_socketPath << std::endl;
    return true;
}

void IPCServer::stop() {
    if (!m_running) return;

    m_running = false;

    // Wake epoll by closing the epoll fd (causes epoll_wait to return)
    if (m_epollFd >= 0) {
        close(m_epollFd);
        m_epollFd = -1;
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }

    // Close all connections
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& [fd, conn] : m_connections) {
            close(fd);
        }
        m_connections.clear();
        m_controlFd = -1;
    }

    if (m_listenFd >= 0) {
        close(m_listenFd);
        m_listenFd = -1;
    }

    if (!m_socketPath.empty()) {
        unlink(m_socketPath.c_str());
    }

    std::cout << "[IPCServer] Stopped" << std::endl;
}

//=============================================================================
// Event Loop
//=============================================================================

void IPCServer::eventLoop() {
    struct epoll_event events[MAX_EVENTS];

    while (m_running) {
        int nfds = epoll_wait(m_epollFd, events, MAX_EVENTS, 500 /* 500ms timeout */);

        if (nfds < 0) {
            if (errno == EINTR) continue;
            if (!m_running) break;  // epoll_fd closed during stop()
            LOG_ERROR("[IPCServer] epoll_wait() failed: " << strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == m_listenFd) {
                acceptClient();
            } else {
                handleReadable(events[i].data.fd);
            }
        }
    }
}

void IPCServer::acceptClient() {
    while (true) {
        int clientFd = accept(m_listenFd, nullptr, nullptr);
        if (clientFd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            LOG_ERROR("[IPCServer] accept() failed: " << strerror(errno));
            break;
        }

        setNonBlocking(clientFd);

        auto conn = std::make_shared<Connection>(clientFd);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_connections[clientFd] = conn;
        }

        // Add to epoll
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLRDHUP;
        ev.data.fd = clientFd;
        if (epoll_ctl(m_epollFd, EPOLL_CTL_ADD, clientFd, &ev) < 0) {
            LOG_ERROR("[IPCServer] epoll_ctl add client failed: " << strerror(errno));
            close(clientFd);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_connections.erase(clientFd);
        }

        LOG_DEBUG("[IPCServer] Client connected (fd=" << clientFd << ")");
    }
}

void IPCServer::handleReadable(int fd) {
    char buf[READ_BUF_SIZE];
    ssize_t n = read(fd, buf, sizeof(buf));

    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        // Connection closed or error
        removeConnection(fd);
        return;
    }

    std::shared_ptr<Connection> conn;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_connections.find(fd);
        if (it == m_connections.end()) return;
        conn = it->second;
    }

    conn->buffer.append(buf, n);

    // Process complete lines
    size_t pos;
    while ((pos = conn->buffer.find('\n')) != std::string::npos) {
        std::string line = conn->buffer.substr(0, pos);
        conn->buffer.erase(0, pos + 1);

        // Trim trailing \r
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (!line.empty()) {
            processLine(fd, line);
        }
    }

    // Safety: prevent unbounded buffer growth from malformed clients
    if (conn->buffer.size() > 65536) {
        LOG_WARN("[IPCServer] Client fd=" << fd << " buffer overflow, disconnecting");
        removeConnection(fd);
    }
}

void IPCServer::removeConnection(int fd) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_connections.find(fd);
    if (it != m_connections.end()) {
        if (it->second->isControl) {
            m_controlFd = -1;
            LOG_DEBUG("[IPCServer] Control connection released (fd=" << fd << ")");
        }
        m_connections.erase(it);
    }

    epoll_ctl(m_epollFd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);

    LOG_DEBUG("[IPCServer] Client disconnected (fd=" << fd << ")");
}

//=============================================================================
// Command Processing
//=============================================================================

void IPCServer::processLine(int fd, const std::string& line) {
    // Extract command
    std::string cmd = jsonGetString(line, "cmd");

    if (cmd.empty()) {
        sendJson(fd, buildError("missing 'cmd' field"));
        return;
    }

    // Status command: available to all connections
    if (cmd == CMD_STATUS) {
        sendStatus(fd);
        return;
    }

    // Discover targets: available to all connections
    if (cmd == CMD_DISCOVER_TARGETS) {
        if (m_callbacks.onDiscoverTargets) {
            auto targets = m_callbacks.onDiscoverTargets();
            sendJson(fd, buildTargetsJson(targets));
        } else {
            sendJson(fd, buildError("discover_targets not available"));
        }
        return;
    }

    // Control commands: require control role
    if (cmd == CMD_ACQUIRE_CONTROL) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_controlFd >= 0 && m_controlFd != fd) {
            sendJson(fd, buildError("control already held by another connection"));
            return;
        }
        m_controlFd = fd;
        auto it = m_connections.find(fd);
        if (it != m_connections.end()) {
            it->second->isControl = true;
        }
        sendJson(fd, buildOk());
        LOG_INFO("[IPCServer] Control acquired by fd=" << fd);
        return;
    }

    if (cmd == CMD_RELEASE_CONTROL) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_controlFd == fd) {
            m_controlFd = -1;
            auto it = m_connections.find(fd);
            if (it != m_connections.end()) {
                it->second->isControl = false;
            }
        }
        sendJson(fd, buildOk());
        return;
    }

    // Check control permission
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_controlFd != fd) {
            sendJson(fd, buildError("not a control connection (send acquire_control first)"));
            return;
        }
    }

    // Dispatch control commands
    if (cmd == CMD_PLAY) {
        std::string path = jsonGetString(line, "path");
        std::string metadata = jsonGetString(line, "metadata");
        if (!path.empty()) {
            if (m_callbacks.onPlay) {
                if (m_callbacks.onPlay(path, metadata)) {
                    sendJson(fd, buildOk());
                } else {
                    sendJson(fd, buildError("play failed"));
                }
            } else {
                sendJson(fd, buildError("play not available"));
            }
        } else {
            // No path = resume from pause
            if (m_callbacks.onResume) {
                sendJson(fd, buildOk());
                m_callbacks.onResume();
            } else {
                sendJson(fd, buildError("resume not available"));
            }
        }
    } else if (cmd == CMD_SET_URI) {
        std::string path = jsonGetString(line, "path");
        std::string metadata = jsonGetString(line, "metadata");
        if (path.empty()) {
            sendJson(fd, buildError("missing 'path' field"));
            return;
        }
        if (m_callbacks.onSetUri) {
            if (m_callbacks.onSetUri(path, metadata)) {
                sendJson(fd, buildOk());
            } else {
                sendJson(fd, buildError("set_uri failed"));
            }
        } else {
            sendJson(fd, buildError("set_uri not available"));
        }
    } else if (cmd == CMD_QUEUE_NEXT) {
        std::string path = jsonGetString(line, "path");
        std::string metadata = jsonGetString(line, "metadata");
        if (path.empty()) {
            sendJson(fd, buildError("missing 'path' field"));
            return;
        }
        if (m_callbacks.onQueueNext) {
            if (m_callbacks.onQueueNext(path, metadata)) {
                sendJson(fd, buildOk());
            } else {
                sendJson(fd, buildError("queue_next failed"));
            }
        } else {
            sendJson(fd, buildError("queue_next not available"));
        }
    } else if (cmd == CMD_PLAY_NOW) {
        std::string path = jsonGetString(line, "path");
        std::string metadata = jsonGetString(line, "metadata");
        if (path.empty()) {
            sendJson(fd, buildError("missing 'path' field"));
            return;
        }
        if (m_callbacks.onPlayNow) {
            if (m_callbacks.onPlayNow(path, metadata)) {
                sendJson(fd, buildOk());
            } else {
                sendJson(fd, buildError("play_now failed"));
            }
        } else {
            sendJson(fd, buildError("play_now not available"));
        }
    } else if (cmd == CMD_PAUSE) {
        if (m_callbacks.onPause) {
            sendJson(fd, buildOk());
            m_callbacks.onPause();
        } else {
            sendJson(fd, buildError("pause not available"));
        }
    } else if (cmd == CMD_STOP) {
        if (m_callbacks.onStop) {
            sendJson(fd, buildOk());
            m_callbacks.onStop();
        } else {
            sendJson(fd, buildError("stop not available"));
        }
    } else if (cmd == CMD_SEEK) {
        double position = jsonGetNumber(line, "position", -1.0);
        if (position < 0) {
            sendJson(fd, buildError("missing or invalid 'position' field"));
            return;
        }
        if (m_callbacks.onSeek) {
            sendJson(fd, buildOk());
            m_callbacks.onSeek(position);
        } else {
            sendJson(fd, buildError("seek not available"));
        }
    } else if (cmd == CMD_SHUTDOWN) {
        sendJson(fd, buildOk());
        if (m_callbacks.onShutdown) {
            m_callbacks.onShutdown();
        }
    } else if (cmd == CMD_SELECT_TARGET) {
        std::string indexStr = jsonGetString(line, "target");
        if (indexStr.empty()) {
            sendJson(fd, buildError("missing 'target' field (1-based index)"));
            return;
        }
        int targetIndex = std::atoi(indexStr.c_str()) - 1;
        if (targetIndex < 0) {
            sendJson(fd, buildError("invalid target index (must be >= 1)"));
            return;
        }
        if (m_callbacks.onSelectTarget) {
            if (m_callbacks.onSelectTarget(targetIndex)) {
                sendJson(fd, buildOk());
            } else {
                sendJson(fd, buildError("Failed to enable target #" + std::to_string(targetIndex + 1)));
            }
        } else {
            sendJson(fd, buildError("select_target not available"));
        }
    } else {
        sendJson(fd, buildError("unknown command: " + cmd));
    }
}

//=============================================================================
// Notifications
//=============================================================================

void IPCServer::notifyStateChange(const std::string& state) {
    std::string json = buildEvent("state_change", "\"transport\":\"" + state + "\"");
    broadcast(json);
}

void IPCServer::notifyTrackChange(const std::string& path, uint32_t sampleRate, uint32_t bitDepth,
                                   uint32_t channels, const std::string& format, double duration) {
    char fields[1024];
    snprintf(fields, sizeof(fields),
        "\"path\":\"%s\",\"sampleRate\":%u,\"bitDepth\":%u,\"channels\":%u,\"format\":\"%s\",\"duration\":%.1f",
        path.c_str(), sampleRate, bitDepth, channels, format.c_str(), duration);
    broadcastBestEffort(buildEvent("track_change", fields));
}

void IPCServer::notifyPosition(double position, double duration) {
    // Use try_lock to avoid blocking the audio thread
    std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;  // Drop notification on contention (best-effort)

    if (m_connections.empty()) return;

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"event\":\"position\",\"position\":%.1f,\"duration\":%.1f}", position, duration);
    std::string json(buf);

    for (auto& [fd, conn] : m_connections) {
        ::send(fd, json.c_str(), json.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        ::send(fd, "\n", 1, MSG_NOSIGNAL | MSG_DONTWAIT);
    }
}

void IPCServer::setCallbacks(const Callbacks& callbacks) {
    m_callbacks = callbacks;
}

//=============================================================================
// Internal Helpers
//=============================================================================

void IPCServer::sendJson(int fd, const std::string& json) {
    std::string msg = json + "\n";
    ::send(fd, msg.c_str(), msg.size(), MSG_NOSIGNAL);
}

void IPCServer::broadcast(const std::string& json) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_connections.empty()) return;

    std::string msg = json + "\n";
    for (auto& [fd, conn] : m_connections) {
        ::send(fd, msg.c_str(), msg.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    }
}

void IPCServer::broadcastBestEffort(const std::string& json) {
    std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;

    if (m_connections.empty()) return;

    std::string msg = json + "\n";
    for (auto& [fd, conn] : m_connections) {
        ::send(fd, msg.c_str(), msg.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    }
}

void IPCServer::sendStatus(int fd) {
    if (!m_statusProvider) {
        sendJson(fd, buildError("status not available"));
        return;
    }

    StatusSnapshot snapshot = m_statusProvider();
    sendJson(fd, buildOkWithStatus(snapshot));
}

//=============================================================================
// JSON Helpers
//=============================================================================

std::string IPCServer::buildStatusJson(const StatusSnapshot& s) {
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"transport\":\"%s\",\"path\":\"%s\",\"position\":%.1f,\"duration\":%.1f,"
        "\"sampleRate\":%u,\"bitDepth\":%u,\"channels\":%u,\"format\":\"%s\","
        "\"dsdRate\":%d,\"bufferLevel\":%.2f,\"trackNumber\":%d}",
        s.transport.c_str(), s.path.c_str(), s.position, s.duration,
        s.sampleRate, s.bitDepth, s.channels, s.format.c_str(),
        s.dsdRate, s.bufferLevel, s.trackNumber);
    return buf;
}

std::string IPCServer::buildTargetsJson(const std::vector<TargetInfo>& targets) {
    // Build: {"ok":true,"targets":[...],"count":N}
    std::string json = "{\"ok\":true,\"targets\":[";

    for (size_t i = 0; i < targets.size(); i++) {
        const auto& t = targets[i];
        if (i > 0) json += ",";

        char buf[512];
        snprintf(buf, sizeof(buf),
            "{\"index\":%d,\"name\":\"%s\",\"output\":\"%s\","
            "\"portIn\":%u,\"portOut\":%u,\"multiport\":%s,"
            "\"config\":\"%s\",\"version\":%u,\"productId\":\"0x%lx\"}",
            t.index,
            t.name.c_str(),
            t.output.c_str(),
            t.portIn, t.portOut,
            t.multiport ? "true" : "false",
            t.config.c_str(),
            t.version,
            (unsigned long)t.productId);
        json += buf;
    }

    char tail[64];
    snprintf(tail, sizeof(tail), "],\"count\":%zu}", targets.size());
    json += tail;

    return json;
}

std::string IPCServer::buildOk() {
    return "{\"ok\":true}";
}

std::string IPCServer::buildOkWithStatus(const StatusSnapshot& s) {
    return "{\"ok\":true,\"status\":" + buildStatusJson(s) + "}";
}

std::string IPCServer::buildError(const std::string& message) {
    // Escape any quotes in the message (simple approach: replace " with ')
    std::string safe = message;
    std::replace(safe.begin(), safe.end(), '"', '\'');
    return "{\"ok\":false,\"error\":\"" + safe + "\"}";
}

std::string IPCServer::buildEvent(const std::string& eventName, const std::string& fields) {
    return "{\"event\":\"" + eventName + "\"," + fields + "}";
}

//=============================================================================
// Minimal JSON Field Extractors
//=============================================================================

std::string IPCServer::jsonGetString(const std::string& json, const std::string& key) {
    // Find "key":"value" pattern
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos) return "";

    // Find the colon after the key
    size_t colonPos = json.find(':', keyPos + searchKey.size());
    if (colonPos == std::string::npos) return "";

    // Skip whitespace after colon
    size_t valueStart = colonPos + 1;
    while (valueStart < json.size() && (json[valueStart] == ' ' || json[valueStart] == '\t')) {
        valueStart++;
    }

    if (valueStart >= json.size()) return "";

    // String value
    if (json[valueStart] == '"') {
        size_t valueEnd = json.find('"', valueStart + 1);
        if (valueEnd == std::string::npos) return "";
        return json.substr(valueStart + 1, valueEnd - valueStart - 1);
    }

    // Non-string value (true, false, null, number) — return as string
    size_t valueEnd = valueStart;
    while (valueEnd < json.size() && json[valueEnd] != ',' && json[valueEnd] != '}') {
        valueEnd++;
    }
    std::string val = json.substr(valueStart, valueEnd - valueStart);
    // Trim whitespace
    while (!val.empty() && val.back() == ' ') val.pop_back();
    return val;
}

double IPCServer::jsonGetNumber(const std::string& json, const std::string& key, double defaultVal) {
    std::string val = jsonGetString(json, key);
    if (val.empty()) return defaultVal;
    try {
        return std::stod(val);
    } catch (...) {
        return defaultVal;
    }
}

bool IPCServer::jsonHasKey(const std::string& json, const std::string& key) {
    return json.find("\"" + key + "\"") != std::string::npos;
}
