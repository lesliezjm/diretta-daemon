/**
 * @file DirettaRenderer.cpp
 * @brief Diretta Host Daemon - IPC controlled audio renderer
 *
 * Refactored from DirettaRendererUPnP. UPnP layer replaced with Unix socket IPC.
 * Audio callback, DirettaSync lifecycle, and flow control preserved exactly.
 */

#include "DirettaRenderer.h"
#include "DirettaSync.h"
#include "IPCServer.h"
#include "AudioEngine.h"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <functional>
#include <unistd.h>
#include <cstring>
#include <pthread.h>
#include <sched.h>

// Logging: uses centralized LogLevel system from LogLevel.h (included via DirettaSync.h)
// DEBUG_LOG kept as alias for backward compatibility within this file
#define DEBUG_LOG(x) LOG_DEBUG(x)

static bool pinThreadToCore(int core, const char* threadName) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    int ret = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (ret != 0) {
        std::cerr << "[" << threadName << "] Failed to set CPU affinity to core "
                  << core << ": " << strerror(ret) << std::endl;
        return false;
    }
    std::cout << "[" << threadName << "] Pinned to CPU core " << core << std::endl;
    return true;
}

//=============================================================================
// Hybrid Flow Control Constants
//=============================================================================

namespace FlowControl {
    constexpr int MICROSLEEP_US = 500;                                    // 500µs micro-sleep (was 10ms)
    constexpr int MAX_WAIT_MS = 20;                                       // Max total wait time
    constexpr int MAX_RETRIES = MAX_WAIT_MS * 1000 / MICROSLEEP_US;       // 40 retries
    constexpr float CRITICAL_BUFFER_LEVEL = 0.10f;                        // Early-return below 10%
}

//=============================================================================
// Auto-release: free Diretta target after idle for coexistence
//=============================================================================

static constexpr int IDLE_RELEASE_TIMEOUT_S = 5;

static PcmOutputMode parsePcmOutputMode(const std::string& mode) {
    if (mode.empty() || mode == "auto") return PcmOutputMode::AUTO;
    if (mode == "force16") return PcmOutputMode::FORCE_16;
    if (mode == "force24") return PcmOutputMode::FORCE_24;
    if (mode == "force32") return PcmOutputMode::FORCE_32;
    if (mode == "prefer32") return PcmOutputMode::PREFER_32;
    return PcmOutputMode::AUTO;
}

//=============================================================================
// Config
//=============================================================================

DirettaRenderer::Config::Config() {
}

//=============================================================================
// Constructor / Destructor
//=============================================================================

DirettaRenderer::DirettaRenderer(const Config& config)
    : m_config(config)
{
    DEBUG_LOG("[DirettaRenderer] Created");
}

DirettaRenderer::~DirettaRenderer() {
    stop();
    DEBUG_LOG("[DirettaRenderer] Destroyed");
}

void DirettaRenderer::waitForCallbackComplete() {
    m_shutdownRequested.store(true, std::memory_order_release);

    auto start = std::chrono::steady_clock::now();
    while (m_callbackRunning.load(std::memory_order_acquire)) {
        std::this_thread::yield();
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(5)) {
            std::cerr << "[DirettaRenderer] CRITICAL: Callback timeout!" << std::endl;
            break;
        }
    }

    m_shutdownRequested.store(false, std::memory_order_release);
}

//=============================================================================
// Status Snapshot
//=============================================================================

IPCServer::StatusSnapshot DirettaRenderer::buildStatusSnapshot() {
    IPCServer::StatusSnapshot s;

    if (!m_audioEngine) {
        s.transport = "stopped";
        return s;
    }

    auto state = m_audioEngine->getState();
    s.transport = (state == AudioEngine::State::PLAYING) ? "playing" :
                  (state == AudioEngine::State::PAUSED) ? "paused" : "stopped";

    const auto& info = m_audioEngine->getCurrentTrackInfo();
    s.path = info.uri;
    s.position = m_audioEngine->getPosition();
    s.duration = (info.sampleRate > 0) ? static_cast<double>(info.duration) / info.sampleRate : 0.0;
    s.sampleRate = info.sampleRate;
    s.bitDepth = info.bitDepth;
    s.channels = info.channels;
    s.format = info.isDSD ? "DSD" : "PCM";
    s.dsdRate = info.isDSD ? info.dsdRate : 0;
    s.bufferLevel = m_direttaSync ? m_direttaSync->getBufferLevel() : 0.0f;
    s.trackNumber = m_audioEngine->getTrackNumber();

    return s;
}

//=============================================================================
// Playback Helpers
//=============================================================================

void DirettaRenderer::stopForUriChangeLocked() {
    auto currentState = m_audioEngine->getState();
    if (currentState != AudioEngine::State::PLAYING &&
        currentState != AudioEngine::State::PAUSED) {
        return;
    }

    std::cout << "[DirettaRenderer] Auto-STOP before URI change" << std::endl;
    m_lastStopTime = std::chrono::steady_clock::now();

    m_audioEngine->stop();
    waitForCallbackComplete();

    if (m_direttaSync && m_direttaSync->isOpen()) {
        m_direttaSync->sendPreTransitionSilence();
        m_direttaSync->stopPlayback(true);
    }

    if (m_ipc) m_ipc->notifyStateChange("stopped");
}

bool DirettaRenderer::setUriLocked(const std::string& path, const std::string& metadata, bool stopIfActive) {
    if (!m_audioEngine || path.empty()) {
        return false;
    }

    if (stopIfActive) {
        stopForUriChangeLocked();
    }

    m_currentURI = path;
    m_currentMetadata = metadata;
    m_audioEngine->setCurrentURI(path, metadata);
    return true;
}

bool DirettaRenderer::queueNextLocked(const std::string& path, const std::string& metadata) {
    if (!m_audioEngine || path.empty()) {
        return false;
    }

    if (!m_currentURI.empty() && path == m_currentURI) {
        DEBUG_LOG("[DirettaRenderer] Next URI same as current, ignoring");
        return true;
    }

    std::cout << "[DirettaRenderer] Queue next: " << path << std::endl;
    m_audioEngine->setNextURI(path, metadata);
    return true;
}

bool DirettaRenderer::playCurrentLocked() {
    if (!m_audioEngine || !m_direttaSync || m_currentURI.empty()) {
        return false;
    }

    if (!m_direttaSync->isEnabled()) {
        std::cerr << "[DirettaRenderer] No Diretta target selected" << std::endl;
        return false;
    }

    m_idleTimerActive.store(false, std::memory_order_release);
    m_direttaReleased.store(false, std::memory_order_release);

    if (m_audioEngine->getState() == AudioEngine::State::PLAYING) {
        DEBUG_LOG("[DirettaRenderer] Already playing, ignoring play");
        return true;
    }

    if (m_direttaSync->isOpen() && m_direttaSync->isPaused()) {
        DEBUG_LOG("[DirettaRenderer] Resuming from pause");
        m_direttaSync->resumePlayback();
        if (!m_audioEngine->play()) {
            if (m_ipc) m_ipc->notifyStateChange("stopped");
            return false;
        }
        if (m_ipc) m_ipc->notifyStateChange("playing");
        return true;
    }

    if (!m_direttaSync->isOpen()) {
        DEBUG_LOG("[DirettaRenderer] Reopening track");
        m_audioEngine->setCurrentURI(m_currentURI, m_currentMetadata, true);
    }

    auto now = std::chrono::steady_clock::now();
    auto timeSinceStop = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastStopTime);
    if (timeSinceStop.count() < 100) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!m_audioEngine->play()) {
        std::cerr << "[DirettaRenderer] AudioEngine::play() failed" << std::endl;
        if (m_ipc) m_ipc->notifyStateChange("stopped");
        return false;
    }

    if (m_ipc) m_ipc->notifyStateChange("playing");
    return true;
}

bool DirettaRenderer::replaceAndPlayLocked(const std::string& path, const std::string& metadata) {
    if (!setUriLocked(path, metadata, true)) {
        return false;
    }
    return playCurrentLocked();
}

bool DirettaRenderer::playPathLocked(const std::string& path, const std::string& metadata) {
    if (!m_audioEngine || path.empty()) {
        return false;
    }

    auto currentState = m_audioEngine->getState();
    if (currentState == AudioEngine::State::PLAYING) {
        if (path == m_currentURI) {
            DEBUG_LOG("[DirettaRenderer] Same path already playing, ignoring play(path)");
            return true;
        }
        return queueNextLocked(path, metadata);
    }

    if (currentState == AudioEngine::State::PAUSED && path == m_currentURI) {
        return playCurrentLocked();
    }

    return replaceAndPlayLocked(path, metadata);
}

//=============================================================================
// Start
//=============================================================================

bool DirettaRenderer::start(std::atomic<bool>* stopSignal) {
    if (m_running) {
        std::cerr << "[DirettaRenderer] Already running" << std::endl;
        return false;
    }

    DEBUG_LOG("[DirettaRenderer] Starting...");

    try {
        //=====================================================================
        // Create and enable DirettaSync (PRESERVED EXACTLY)
        //=====================================================================

        m_direttaSync = std::make_unique<DirettaSync>();
        m_direttaSync->setTargetIndex(m_config.targetIndex);

        // Build sync config (used by selectTarget and startup enable)
        m_syncConfig = std::make_unique<DirettaConfig>();
        if (m_config.threadMode >= 0)
            m_syncConfig->threadMode = m_config.threadMode;
        if (m_config.cycleTime >= 0) {
            m_syncConfig->cycleTime = static_cast<unsigned int>(m_config.cycleTime);
            m_syncConfig->cycleTimeAuto = false;
        }
        if (m_config.infoCycle >= 0)
            m_syncConfig->infoCycle = static_cast<unsigned int>(m_config.infoCycle);
        if (m_config.cycleMinTime >= 0)
            m_syncConfig->cycleMinTime = static_cast<unsigned int>(m_config.cycleMinTime);
        if (m_config.mtu >= 0)
            m_syncConfig->mtu = static_cast<unsigned int>(m_config.mtu);
        if (!m_config.transferMode.empty()) {
            if (m_config.transferMode == "varmax")
                m_syncConfig->transferMode = DirettaTransferMode::VAR_MAX;
            else if (m_config.transferMode == "varauto")
                m_syncConfig->transferMode = DirettaTransferMode::VAR_AUTO;
            else if (m_config.transferMode == "fixauto")
                m_syncConfig->transferMode = DirettaTransferMode::FIX_AUTO;
            else if (m_config.transferMode == "random")
                m_syncConfig->transferMode = DirettaTransferMode::RANDOM;
            else
                m_syncConfig->transferMode = DirettaTransferMode::AUTO;
        }
        if (m_config.targetProfileLimitTime >= 0)
            m_syncConfig->targetProfileLimitTime = static_cast<unsigned int>(m_config.targetProfileLimitTime);
        m_syncConfig->pcmOutputMode = parsePcmOutputMode(m_config.pcmOutputMode);
        if (m_config.cpuAudio >= 0)
            m_syncConfig->cpuAudio = m_config.cpuAudio;
        if (m_config.cpuOther >= 0)
            m_syncConfig->cpuOther = m_config.cpuOther;

        // Log non-default SDK settings
        if (m_config.threadMode >= 0)
            std::cout << "[DirettaRenderer] Thread mode: " << m_syncConfig->threadMode << std::endl;
        if (m_config.cycleTime >= 0)
            std::cout << "[DirettaRenderer] Cycle time: " << m_syncConfig->cycleTime << " us (auto disabled)" << std::endl;
        if (m_config.infoCycle >= 0)
            std::cout << "[DirettaRenderer] Info cycle: " << m_syncConfig->infoCycle << " us" << std::endl;
        if (m_config.cycleMinTime >= 0)
            std::cout << "[DirettaRenderer] Cycle min time: " << m_syncConfig->cycleMinTime << " us" << std::endl;
        if (!m_config.transferMode.empty())
            std::cout << "[DirettaRenderer] Transfer mode: " << m_config.transferMode << std::endl;
        if (m_config.mtu >= 0)
            std::cout << "[DirettaRenderer] MTU override: " << m_syncConfig->mtu << std::endl;
        if (m_config.targetProfileLimitTime >= 0)
            std::cout << "[DirettaRenderer] Target profile limit: " << m_syncConfig->targetProfileLimitTime
                      << " us (" << (m_syncConfig->targetProfileLimitTime > 0 ? "TargetProfile" : "SelfProfile") << ")" << std::endl;
        if (m_config.pcmOutputMode != "auto")
            std::cout << "[DirettaRenderer] PCM output mode: " << m_config.pcmOutputMode << std::endl;
        if (m_config.cpuAudio >= 0)
            std::cout << "[DirettaRenderer] CPU audio (Diretta worker): core " << m_config.cpuAudio << std::endl;
        if (m_config.cpuOther >= 0)
            std::cout << "[DirettaRenderer] CPU other (decode/IPC): core " << m_config.cpuOther << std::endl;

        // If target specified at startup, enable immediately (backward compat)
        // Otherwise, daemon starts idle — use select_target via IPC
        if (m_config.targetIndex >= 0) {
            if (!selectTarget(m_config.targetIndex, stopSignal)) {
                return false;
            }
        } else {
            std::cout << "[DirettaRenderer] No target specified — use IPC select_target to choose" << std::endl;
        }

        //=====================================================================
        // Create IPC Server (replaces UPnPDevice)
        //=====================================================================

        m_ipc = std::make_unique<IPCServer>();

        // Create AudioEngine
        m_audioEngine = std::make_unique<AudioEngine>();

        //=====================================================================
        // Audio Callback (PRESERVED EXACTLY)
        //=====================================================================

        m_audioEngine->setAudioCallback(
            [this](const AudioBuffer& buffer, size_t samples,
                   uint32_t sampleRate, uint32_t bitDepth, uint32_t channels) -> bool {

                // Check if shutdown requested (avoid work during teardown)
                if (m_shutdownRequested.load(std::memory_order_acquire)) {
                    return false;
                }

                // Lightweight atomic flag (no syscalls in hot path)
                m_callbackRunning.store(true, std::memory_order_release);
                struct Guard {
                    std::atomic<bool>& flag;
                    ~Guard() { flag.store(false, std::memory_order_release); }
                } guard{m_callbackRunning};

                const TrackInfo& trackInfo = m_audioEngine->getCurrentTrackInfo();

                // Build format
                AudioFormat format(sampleRate, bitDepth, channels);
                format.isDSD = trackInfo.isDSD;
                format.isCompressed = trackInfo.isCompressed;
                format.isRemoteStream = trackInfo.isRemoteStream;

                if (trackInfo.isDSD) {
                    format.bitDepth = 1;
                    // Use detected source format (from file extension or codec)
                    if (trackInfo.dsdSourceFormat == TrackInfo::DSDSourceFormat::DSF) {
                        format.dsdFormat = AudioFormat::DSDFormat::DSF;
                        DEBUG_LOG("[Callback] DSD format: DSF (LSB first)");
                    } else if (trackInfo.dsdSourceFormat == TrackInfo::DSDSourceFormat::DFF) {
                        format.dsdFormat = AudioFormat::DSDFormat::DFF;
                        DEBUG_LOG("[Callback] DSD format: DFF (MSB first)");
                    } else {
                        // Fallback to codec string if detection failed
                        format.dsdFormat = (trackInfo.codec.find("lsb") != std::string::npos)
                            ? AudioFormat::DSDFormat::DSF
                            : AudioFormat::DSDFormat::DFF;
                        DEBUG_LOG("[Callback] DSD format: "
                                  << (format.dsdFormat == AudioFormat::DSDFormat::DSF ? "DSF" : "DFF")
                                  << " (from codec fallback)");
                    }
                }

                // Open/resume connection if needed
                // Check isPlaying() not isOpen() - after stopPlayback(), isOpen() is true
                // but we still need to call open() to trigger quick resume
                //
                // CRITICAL FIX: Also check for format changes!
                // When transitioning DSD→PCM (or vice versa), DirettaSync may still be
                // "playing" but with the wrong format. We must call open() to reconfigure.
                bool needsOpen = !m_direttaSync->isPlaying();

                if (!needsOpen && m_direttaSync->isOpen()) {
                    // Check if format has changed
                    const AudioFormat& currentSyncFormat = m_direttaSync->getFormat();
                    bool formatChanged = (currentSyncFormat.sampleRate != format.sampleRate ||
                                         currentSyncFormat.bitDepth != format.bitDepth ||
                                         currentSyncFormat.channels != format.channels ||
                                         currentSyncFormat.isDSD != format.isDSD);
                    if (formatChanged) {
                        std::cout << "[Callback] FORMAT CHANGE DETECTED!" << std::endl;
                        std::cout << "[Callback]   Old: " << currentSyncFormat.sampleRate << "Hz/"
                                  << currentSyncFormat.bitDepth << "bit "
                                  << (currentSyncFormat.isDSD ? "DSD" : "PCM") << std::endl;
                        std::cout << "[Callback]   New: " << format.sampleRate << "Hz/"
                                  << format.bitDepth << "bit "
                                  << (format.isDSD ? "DSD" : "PCM") << std::endl;

                        // v2.0.1 FIX: Use stopPlayback(false) to send silence before stopping
                        // This flushes the Diretta pipeline and prevents crackling on format transitions
                        // With immediate=true, no silence was sent, causing DAC sync issues
                        m_direttaSync->stopPlayback(false);
                        needsOpen = true;
                    }
                }

                if (needsOpen) {
                    if (!m_direttaSync->open(format)) {
                        std::cerr << "[Callback] Failed to open DirettaSync" << std::endl;
                        return false;
                    }
                    m_lastS24HintKey.clear();
                }

                // Propagate S24 alignment hint once per PCM track, even when
                // gapless/same-format promotion avoids a DirettaSync::open().
                std::string s24HintKey = trackInfo.uri + "|" +
                    std::to_string(sampleRate) + "|" +
                    std::to_string(bitDepth) + "|" +
                    std::to_string(channels);
                if (!format.isDSD && s24HintKey != m_lastS24HintKey) {
                    m_lastS24HintKey = s24HintKey;
                    if (bitDepth == 24 &&
                        trackInfo.s24Alignment != TrackInfo::S24Alignment::Unknown) {
                        DirettaRingBuffer::S24PackMode hint =
                            (trackInfo.s24Alignment == TrackInfo::S24Alignment::LsbAligned)
                                ? DirettaRingBuffer::S24PackMode::LsbAligned
                                : DirettaRingBuffer::S24PackMode::MsbAligned;
                        m_direttaSync->setS24PackModeHint(hint);
                        DEBUG_LOG("[Callback] Set S24 hint: "
                                  << (hint == DirettaRingBuffer::S24PackMode::LsbAligned ? "LSB" : "MSB"));
                    } else {
                        m_direttaSync->resetS24PackMode();
                    }
                }

                // Send audio (DirettaSync handles all format conversions)
                if (trackInfo.isDSD) {
                    // DSD: Atomic send with event-based flow control (G1)
                    // Uses condition variable instead of blocking 5ms sleep
                    // Reduces jitter from ±2.5ms to ±50µs
                    int retryCount = 0;
                    const int maxRetries = 20;  // Reduced: each wait is ~500µs max
                    size_t sent = 0;

                    while (sent == 0 && retryCount < maxRetries) {
                        sent = m_direttaSync->sendAudio(buffer.data(), samples);

                        if (sent == 0) {
                            // Event-based wait: wake on space available or 500µs timeout
                            std::unique_lock<std::mutex> lock(m_direttaSync->getFlowMutex());
                            m_direttaSync->waitForSpace(lock, std::chrono::microseconds(500));
                            retryCount++;
                        }
                    }

                    if (sent == 0) {
                        std::cerr << "[Callback] DSD timeout after " << retryCount << " retries" << std::endl;
                    }
                } else {
                    // PCM: Incremental send with hybrid flow control
                    const uint8_t* audioData = buffer.data();
                    size_t remainingSamples = samples;
                    size_t bytesPerSample = (bitDepth == 24 || bitDepth == 32)
                        ? 4 * channels : (bitDepth / 8) * channels;

                    // Hybrid flow control: micro-sleep normally, early-return if buffer critical
                    float bufferLevel = m_direttaSync->getBufferLevel();
                    bool criticalMode = (bufferLevel < FlowControl::CRITICAL_BUFFER_LEVEL);

                    int retryCount = 0;

                    while (remainingSamples > 0 && retryCount < FlowControl::MAX_RETRIES) {
                        size_t sent = m_direttaSync->sendAudio(audioData, remainingSamples);

                        if (sent > 0) {
                            size_t samplesConsumed = sent / bytesPerSample;
                            remainingSamples -= samplesConsumed;
                            audioData += sent;
                            retryCount = 0;
                        } else {
                            if (criticalMode) {
                                // Buffer critically low - return immediately to prioritize refill
                                DEBUG_LOG("[Audio] Early-return, buffer critical: " << bufferLevel);
                                break;
                            }
                            // Normal backpressure: 500µs micro-sleep (was 10ms)
                            std::this_thread::sleep_for(std::chrono::microseconds(FlowControl::MICROSLEEP_US));
                            retryCount++;
                        }
                    }
                }

                return true;
            }
        );

        //=====================================================================
        // Track Change Callback (notification target changed: m_ipc)
        //=====================================================================

        m_audioEngine->setTrackChangeCallback(
            [this](int trackNumber, const TrackInfo& info, const std::string& uri, const std::string& metadata) {
                // Keep DirettaRenderer URI in sync with AudioEngine
                // try_to_lock: avoids deadlock when called from onPlay → play() → openCurrentTrack()
                {
                    std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
                    if (lock.owns_lock()) {
                        m_currentURI = uri;
                        m_currentMetadata = metadata;
                    }
                }

                if (g_verbose) {
                    std::cout << "[DirettaRenderer] Track " << trackNumber << ": " << info.codec;
                    if (info.isDSD) {
                        std::cout << " DSD" << info.dsdRate << " (" << info.sampleRate << "Hz)";
                    } else {
                        std::cout << " " << info.sampleRate << "Hz/" << info.bitDepth << "bit";
                    }
                    std::cout << "/" << info.channels << "ch" << std::endl;
                }

                // Notify IPC clients of track change
                double durationSec = (info.sampleRate > 0) ? static_cast<double>(info.duration) / info.sampleRate : 0.0;
                std::string fmt = info.isDSD ? "DSD" : "PCM";
                if (m_ipc) {
                    m_ipc->notifyTrackChange(uri, info.sampleRate, info.bitDepth, info.channels, fmt, durationSec);
                }
            }
        );

        //=====================================================================
        // Track End Callback (notification target changed: m_ipc)
        //=====================================================================

        m_audioEngine->setTrackEndCallback([this]() {
            std::cout << "[DirettaRenderer] Track ended naturally" << std::endl;

            if (m_direttaSync) {
                // Wait for ring buffer to drain before stopping.
                if (m_direttaSync->isPlaying()) {
                    auto drainStart = std::chrono::steady_clock::now();
                    constexpr int DRAIN_TIMEOUT_MS = 2000;
                    constexpr float DRAIN_THRESHOLD = 0.01f;

                    while (m_direttaSync->isPlaying()) {
                        float level = m_direttaSync->getBufferLevel();
                        if (level < DRAIN_THRESHOLD) break;

                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - drainStart);
                        if (elapsed.count() >= DRAIN_TIMEOUT_MS) {
                            std::cerr << "[DirettaRenderer] Drain timeout ("
                                      << DRAIN_TIMEOUT_MS << "ms), level=" << level << std::endl;
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                }

                // Stop with silence tail for clean DAC shutdown
                m_direttaSync->stopPlayback(false);

                // Fully release the Diretta target on playlist end
                m_direttaSync->release();
            }

            // Notify IPC clients that track finished
            if (m_ipc) {
                m_ipc->notifyStateChange("stopped");
            }
        });

        //=====================================================================
        // IPC Command Handlers (replace UPnP callbacks)
        //=====================================================================

        IPCServer::Callbacks ipcCallbacks;

        ipcCallbacks.onPlay = [this](const std::string& path, const std::string& metadata) -> bool {
            std::cout << "[DirettaRenderer] Play: " << path << std::endl;
            std::lock_guard<std::mutex> lock(m_mutex);
            return playPathLocked(path, metadata);
        };

        ipcCallbacks.onSetUri = [this](const std::string& path, const std::string& metadata) -> bool {
            std::cout << "[DirettaRenderer] Set URI: " << path << std::endl;
            std::lock_guard<std::mutex> lock(m_mutex);
            return setUriLocked(path, metadata, true);
        };

        ipcCallbacks.onQueueNext = [this](const std::string& path, const std::string& metadata) -> bool {
            std::lock_guard<std::mutex> lock(m_mutex);
            return queueNextLocked(path, metadata);
        };

        ipcCallbacks.onPlayNow = [this](const std::string& path, const std::string& metadata) -> bool {
            std::cout << "[DirettaRenderer] Play now: " << path << std::endl;
            std::lock_guard<std::mutex> lock(m_mutex);
            return replaceAndPlayLocked(path, metadata);
        };

        ipcCallbacks.onResume = [this]() {
            std::cout << "[DirettaRenderer] Resume" << std::endl;
            std::lock_guard<std::mutex> lock(m_mutex);
            playCurrentLocked();
        };

        ipcCallbacks.onPause = [this]() {
            std::lock_guard<std::mutex> lock(m_mutex);
            std::cout << "[DirettaRenderer] Pause" << std::endl;

            if (m_audioEngine) {
                m_audioEngine->pause();
            }
            if (m_direttaSync && m_direttaSync->isPlaying()) {
                m_direttaSync->pausePlayback();
            }
            if (m_ipc) m_ipc->notifyStateChange("paused");
        };

        ipcCallbacks.onStop = [this]() {
            std::lock_guard<std::mutex> lock(m_mutex);

            // Guard against redundant stop calls
            if (m_direttaSync && !m_direttaSync->isOpen() && !m_direttaSync->isPlaying()) {
                DEBUG_LOG("[DirettaRenderer] Stop ignored - already stopped");
                return;
            }

            std::cout << "[DirettaRenderer] Stop" << std::endl;
            std::cout << "════════════════════════════════════════" << std::endl;

            m_lastStopTime = std::chrono::steady_clock::now();

            m_audioEngine->stop();
            waitForCallbackComplete();

            if (!m_currentURI.empty()) {
                m_audioEngine->setCurrentURI(m_currentURI, m_currentMetadata, true);
            }

            if (m_direttaSync && m_direttaSync->isOpen()) {
                m_direttaSync->stopPlayback(false);
            }

            if (m_ipc) m_ipc->notifyStateChange("stopped");

            // Start idle release timer
            m_idleTimerActive.store(true, std::memory_order_release);
        };

        ipcCallbacks.onSeek = [this](double seconds) {
            std::lock_guard<std::mutex> lock(m_mutex);
            std::cout << "[DirettaRenderer] Seek: " << seconds << "s" << std::endl;

            if (m_audioEngine) {
                m_audioEngine->seek(seconds);
            }
        };

        ipcCallbacks.onShutdown = [this]() {
            std::cout << "[DirettaRenderer] Shutdown requested via IPC" << std::endl;
            // Trigger main loop exit by setting running to false
            m_running = false;
        };

        ipcCallbacks.onDiscoverTargets = []() -> std::vector<IPCServer::TargetInfo> {
            auto raw = DirettaSync::discoverTargets();
            std::vector<IPCServer::TargetInfo> result;
            result.reserve(raw.size());
            for (const auto& t : raw) {
                result.push_back({t.index, t.name, t.output, t.portIn, t.portOut,
                                  t.multiport, t.config, t.version, t.productId});
            }
            return result;
        };

        ipcCallbacks.onSelectTarget = [this](int targetIndex) -> bool {
            return selectTarget(targetIndex);
        };

        m_ipc->setCallbacks(ipcCallbacks);

        // Start IPC server
        if (!m_ipc->start(m_config.socketPath, [this]() { return buildStatusSnapshot(); })) {
            std::cerr << "[DirettaRenderer] Failed to start IPC server on " << m_config.socketPath << std::endl;
            return false;
        }

        // Start threads
        m_running = true;
        m_audioThread = std::thread(&DirettaRenderer::audioThreadFunc, this);
        m_positionThread = std::thread(&DirettaRenderer::positionThreadFunc, this);

        std::cout << "[DirettaRenderer] Started (IPC: " << m_config.socketPath << ")" << std::endl;
        return true;

    } catch (const std::exception& e) {
        std::cerr << "[DirettaRenderer] Exception: " << e.what() << std::endl;
        stop();
        return false;
    }
}

//=============================================================================
// Target Selection
//=============================================================================

bool DirettaRenderer::selectTarget(int targetIndex, std::atomic<bool>* stopSignal) {
    std::cout << "[DirettaRenderer] Selecting target #" << (targetIndex + 1) << "..." << std::endl;

    // Disable existing connection if any
    if (m_direttaSync) {
        m_direttaSync->disable();
    }

    m_direttaSync->setTargetIndex(targetIndex);

    if (!m_direttaSync->enable(*m_syncConfig, stopSignal)) {
        std::cerr << "[DirettaRenderer] Failed to enable target #" << (targetIndex + 1) << std::endl;
        return false;
    }

    std::cout << "[DirettaRenderer] Target #" << (targetIndex + 1) << " connected" << std::endl;

    // Pre-connect warmup to eliminate first-play glitch
    {
        AudioFormat warmupFmt;
        warmupFmt.sampleRate = 44100;
        warmupFmt.bitDepth = 16;
        if (m_config.pcmOutputMode == "force24") {
            warmupFmt.bitDepth = 24;
        } else if (m_config.pcmOutputMode == "force32" ||
                   m_config.pcmOutputMode == "prefer32") {
            warmupFmt.bitDepth = 32;
        }
        warmupFmt.channels = 2;
        warmupFmt.isDSD = false;
        std::cout << "[DirettaRenderer] Pre-connecting (warmup "
                  << warmupFmt.bitDepth << "-bit)..." << std::endl;
        if (m_direttaSync->open(warmupFmt)) {
            m_direttaSync->stopPlayback(true);
            m_lastStopTime = std::chrono::steady_clock::now();
            std::cout << "[DirettaRenderer] Warmup done" << std::endl;
        } else {
            std::cerr << "[DirettaRenderer] Warmup failed (non-fatal)" << std::endl;
        }
    }

    return true;
}

//=============================================================================
// Stop
//=============================================================================

void DirettaRenderer::dumpStats() const {
    if (m_direttaSync) {
        m_direttaSync->dumpStats();
    }
}

void DirettaRenderer::stop() {
    if (!m_running) return;

    DEBUG_LOG("[DirettaRenderer] Stopping...");

    m_running = false;

    if (m_audioEngine) {
        m_audioEngine->stop();
    }

    if (m_direttaSync) {
        m_direttaSync->disable();
    }

    if (m_ipc) {
        m_ipc->stop();
    }

    if (m_audioThread.joinable()) m_audioThread.join();
    if (m_positionThread.joinable()) m_positionThread.join();

    DEBUG_LOG("[DirettaRenderer] Stopped");
}

//=============================================================================
// Thread Functions
//=============================================================================

void DirettaRenderer::audioThreadFunc() {
    if (m_config.cpuOther >= 0) pinThreadToCore(m_config.cpuOther, "Audio Thread");
    DEBUG_LOG("[Audio Thread] Started");

    // Buffer-level flow control thresholds (like MPD's Delay() approach)
    constexpr float BUFFER_HIGH_THRESHOLD = 0.5f;  // Throttle when >50% full
    constexpr float BUFFER_LOW_THRESHOLD = 0.25f;  // Warn when <25% full

    uint32_t lastSampleRate = 0;
    size_t currentSamplesPerCall = 8192;

    while (m_running) {
        if (!m_audioEngine) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        auto state = m_audioEngine->getState();

        if (state == AudioEngine::State::PLAYING) {
            const auto& trackInfo = m_audioEngine->getCurrentTrackInfo();
            uint32_t sampleRate = trackInfo.sampleRate;
            bool isDSD = trackInfo.isDSD;

            if (sampleRate == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            // Adjust samples per call based on format
            // PCM: 2048 samples = ~46ms at 44.1kHz
            // DSD: Rate-adaptive for consistent ~12ms chunks
            size_t samplesPerCall;
            if (isDSD) {
                samplesPerCall = DirettaBuffer::calculateDsdSamplesPerCall(sampleRate);
            } else {
                samplesPerCall = 2048;
            }

            if (sampleRate != lastSampleRate || samplesPerCall != currentSamplesPerCall) {
                currentSamplesPerCall = samplesPerCall;
                lastSampleRate = sampleRate;
                DEBUG_LOG("[Audio Thread] Format: " << sampleRate << "Hz "
                          << (isDSD ? "DSD" : "PCM") << ", samples/call="
                          << currentSamplesPerCall);
            }

            // Buffer-level flow control (MPD-style)
            float bufferLevel = 0.0f;
            if (m_direttaSync && m_direttaSync->isPlaying()) {
                bufferLevel = m_direttaSync->getBufferLevel();
            }

            if (bufferLevel > BUFFER_HIGH_THRESHOLD) {
                // Buffer is healthy - throttle to avoid wasting CPU
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } else {
                // Buffer needs filling - process immediately
                bool success = m_audioEngine->process(currentSamplesPerCall);

                if (!success) {
                    // No data available from decoder, brief pause
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                } else if (bufferLevel < BUFFER_LOW_THRESHOLD && bufferLevel > 0.0f) {
                    // Buffer is getting low - process again immediately (catch up)
                    m_audioEngine->process(currentSamplesPerCall);
                }
            }
        } else {
            // Auto-release Diretta target after idle timeout
            if (m_idleTimerActive.load(std::memory_order_acquire) &&
                !m_direttaReleased.load(std::memory_order_acquire)) {
                auto elapsed = std::chrono::steady_clock::now() - m_lastStopTime;
                if (elapsed >= std::chrono::seconds(IDLE_RELEASE_TIMEOUT_S)) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_direttaSync && m_direttaSync->isOpen()) {
                        std::cout << "[DirettaRenderer] No activity for "
                                  << IDLE_RELEASE_TIMEOUT_S
                                  << "s — releasing Diretta target for other sources"
                                  << std::endl;
                        m_direttaSync->release();
                    }
                    m_direttaReleased.store(true, std::memory_order_release);
                    m_idleTimerActive.store(false, std::memory_order_release);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            lastSampleRate = 0;
        }
    }

    DEBUG_LOG("[Audio Thread] Stopped");
}

void DirettaRenderer::positionThreadFunc() {
    if (m_config.cpuOther >= 0) pinThreadToCore(m_config.cpuOther, "Position Thread");
    DEBUG_LOG("[Position Thread] Started");

    while (m_running) {
        if (!m_audioEngine || !m_ipc) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        auto state = m_audioEngine->getState();

        if (state == AudioEngine::State::PLAYING) {
            double positionSeconds = m_audioEngine->getPosition();
            const auto& trackInfo = m_audioEngine->getCurrentTrackInfo();
            double duration = 0.0;
            if (trackInfo.sampleRate > 0) {
                duration = static_cast<double>(trackInfo.duration) / trackInfo.sampleRate;
            }

            // Cap reported position to (duration - 1) while PLAYING
            if (duration > 0.0 && positionSeconds >= duration) {
                positionSeconds = duration - 1.0;
            }

            m_ipc->notifyPosition(positionSeconds, duration);
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    DEBUG_LOG("[Position Thread] Stopped");
}
