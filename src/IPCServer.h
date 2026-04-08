/**
 * @file IPCServer.h
 * @brief Unix domain socket IPC server for Diretta Renderer
 *
 * Provides a simple JSON-over-Unix-socket interface for external players
 * to control playback. Supports multiple read-only status connections and
 * a single control connection for play/pause/stop/seek commands.
 */

#ifndef IPC_SERVER_H
#define IPC_SERVER_H

#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <memory>
#include <cstdint>

class IPCServer {
public:
    //=========================================================================
    // Callbacks (command handlers registered by DirettaRenderer)
    //=========================================================================

    using PlayCallback = std::function<void(const std::string& path)>;
    using ResumeCallback = std::function<void()>;
    using PauseCallback = std::function<void()>;
    using StopCallback = std::function<void()>;
    using SeekCallback = std::function<void(double seconds)>;
    using ShutdownCallback = std::function<void()>;
    using SelectTargetCallback = std::function<bool(int targetIndex)>;

    struct TargetInfo {
        int index;
        std::string name;
        std::string output;
        uint16_t portIn;
        uint16_t portOut;
        bool multiport;
        std::string config;
        uint16_t version;
        uint64_t productId;
    };

    using DiscoverTargetsCallback = std::function<std::vector<TargetInfo>()>;

    struct Callbacks {
        PlayCallback onPlay;
        ResumeCallback onResume;
        PauseCallback onPause;
        StopCallback onStop;
        SeekCallback onSeek;
        ShutdownCallback onShutdown;
        SelectTargetCallback onSelectTarget;
        DiscoverTargetsCallback onDiscoverTargets;
    };

    //=========================================================================
    // Status Snapshot (returned by status queries)
    //=========================================================================

    struct StatusSnapshot {
        std::string transport;  // "playing", "paused", "stopped"
        std::string path;
        double position = 0.0;
        double duration = 0.0;
        uint32_t sampleRate = 0;
        uint32_t bitDepth = 0;
        uint32_t channels = 0;
        std::string format;     // "PCM" or "DSD"
        int dsdRate = 0;
        float bufferLevel = 0.0f;
        int trackNumber = 0;
    };

    using StatusProvider = std::function<StatusSnapshot()>;

    //=========================================================================
    // Lifecycle
    //=========================================================================

    IPCServer();
    ~IPCServer();

    // Non-copyable
    IPCServer(const IPCServer&) = delete;
    IPCServer& operator=(const IPCServer&) = delete;

    /**
     * @brief Start the IPC server
     * @param socketPath Unix socket path (e.g., "/tmp/diretta-renderer.sock")
     * @param provider Status snapshot provider function
     * @return true if started successfully
     */
    bool start(const std::string& socketPath, StatusProvider provider);

    /**
     * @brief Stop the IPC server and clean up
     */
    void stop();

    //=========================================================================
    // Push Notifications (broadcast to all connected clients)
    //=========================================================================

    void notifyStateChange(const std::string& state);
    void notifyTrackChange(const std::string& path, uint32_t sampleRate, uint32_t bitDepth,
                           uint32_t channels, const std::string& format, double duration);
    void notifyPosition(double position, double duration);

    void setCallbacks(const Callbacks& callbacks);

private:
    //=========================================================================
    // Connection Management
    //=========================================================================

    struct Connection {
        int fd;
        bool isControl;
        std::string buffer;  // Incomplete line buffer

        explicit Connection(int f) : fd(f), isControl(false) {}
    };

    void eventLoop();
    void acceptClient();
    void handleReadable(int fd);
    void processLine(int fd, const std::string& line);
    void removeConnection(int fd);

    void sendJson(int fd, const std::string& json);
    void broadcast(const std::string& json);
    void sendStatus(int fd);

    //=========================================================================
    // JSON Helpers (hand-crafted, no external dependency)
    //=========================================================================

    static std::string buildStatusJson(const StatusSnapshot& s);
    static std::string buildOk();
    static std::string buildOkWithStatus(const StatusSnapshot& s);
    static std::string buildError(const std::string& message);
    static std::string buildEvent(const std::string& eventName, const std::string& fields);
    static std::string buildTargetsJson(const std::vector<TargetInfo>& targets);

    // Minimal JSON field extractor (extracts string/number value for a given key)
    static std::string jsonGetString(const std::string& json, const std::string& key);
    static double jsonGetNumber(const std::string& json, const std::string& key, double defaultVal = 0.0);
    static bool jsonHasKey(const std::string& json, const std::string& key);

    //=========================================================================
    // State
    //=========================================================================

    std::string m_socketPath;
    int m_listenFd = -1;
    int m_epollFd = -1;
    std::atomic<bool> m_running{false};
    std::thread m_thread;

    StatusProvider m_statusProvider;
    Callbacks m_callbacks;

    std::mutex m_mutex;  // Protects m_connections and m_controlFd
    std::unordered_map<int, std::shared_ptr<Connection>> m_connections;
    int m_controlFd = -1;  // -1 = no control connection
};

#endif // IPC_SERVER_H
