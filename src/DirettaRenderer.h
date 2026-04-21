/**
 * @file DirettaRenderer.h
 * @brief Diretta Host Daemon - IPC controlled audio renderer
 *
 * Refactored from DirettaRendererUPnP to provide Unix socket IPC interface
 * for external music player integration. Uses unified DirettaSync class.
 */

#pragma once

#include <memory>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <iostream>

// Forward declarations
class AudioEngine;
class DirettaSync;
struct AudioFormat;
struct DirettaConfig;

#include "IPCServer.h"

class DirettaRenderer {
public:
    struct Config {
        std::string socketPath = "/tmp/diretta-renderer.sock";
        bool gaplessEnabled = true;
        int targetIndex = -1;  // -1 = interactive, >= 0 = specific
        std::string networkInterface;  // Empty = auto-detect

        // Advanced Diretta SDK settings (-1 = use default)
        int threadMode = -1;       // SDK THRED_MODE bitmask (default: 1 = CRITICAL)
        int cycleTime = -1;        // Cycle time in µs (default: 2620, auto-calculated)
        int infoCycle = -1;        // Info packet cycle in µs (default: 100000 = 100ms)
        int cycleMinTime = -1;     // Min cycle time in µs (default: unused, random mode only)
        std::string transferMode;  // Transfer mode: auto|varmax|varauto|fixauto|random
        int mtu = -1;             // MTU override in bytes (default: auto-detect)
        int targetProfileLimitTime = -1;  // 0=SelfProfile (stable, default), >0=TargetProfile limit in µs (experimental)

        // CPU affinity (-1 = no pinning, default)
        int cpuAudio = -1;        // Core for DirettaSync worker thread
        int cpuOther = -1;        // Core for decode/IPC/position/logging threads

        Config();
    };

    DirettaRenderer(const Config& config);
    ~DirettaRenderer();

    bool start(std::atomic<bool>* stopSignal = nullptr);
    void stop();

    bool isRunning() const { return m_running; }

    /** @brief Dump runtime statistics (called by SIGUSR1 handler) */
    void dumpStats() const;

private:
    // Thread functions
    void audioThreadFunc();
    void positionThreadFunc();

    // Helper to wait for audio callback completion
    void waitForCallbackComplete();

    // Build status snapshot for IPC queries
    IPCServer::StatusSnapshot buildStatusSnapshot();

    // Select and connect to a Diretta target (disables previous if any)
    bool selectTarget(int targetIndex, std::atomic<bool>* stopSignal = nullptr);

    // Playback helpers. Call with m_mutex held.
    bool playPathLocked(const std::string& path, const std::string& metadata);
    bool setUriLocked(const std::string& path, const std::string& metadata, bool stopIfActive);
    bool queueNextLocked(const std::string& path, const std::string& metadata);
    bool playCurrentLocked();
    bool replaceAndPlayLocked(const std::string& path, const std::string& metadata);
    void stopForUriChangeLocked();

    // Configuration
    Config m_config;
    std::unique_ptr<struct DirettaConfig> m_syncConfig;

    // Components
    std::unique_ptr<IPCServer> m_ipc;
    std::unique_ptr<AudioEngine> m_audioEngine;
    std::unique_ptr<DirettaSync> m_direttaSync;

    // Threads
    std::thread m_audioThread;
    std::thread m_positionThread;

    // State
    std::atomic<bool> m_running{false};
    std::mutex m_mutex;

    // Current track info
    std::string m_currentURI;
    std::string m_currentMetadata;

    // Callback synchronization (lock-free for hot path)
    std::atomic<bool> m_callbackRunning{false};
    std::atomic<bool> m_shutdownRequested{false};

    // DAC stabilization timing
    std::chrono::steady_clock::time_point m_lastStopTime;

    // Auto-release: free Diretta target after idle timeout for coexistence
    std::atomic<bool> m_idleTimerActive{false};
    std::atomic<bool> m_direttaReleased{false};
};
