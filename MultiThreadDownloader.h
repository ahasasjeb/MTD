#pragma once

#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <chrono>
#include <cstdint>

struct DownloadConfig {
    int threadCount = 8;
    int64_t speedLimit = 0;
    std::string userAgent = "PCL2/MultiThreadDownloader Mozilla/5.0 AppleWebKit/537.36 Chrome/63.0.3239.132 Safari/537.36";
    int maxRedirects = 10;
    int connectTimeout = 10000;
    int readTimeout = 30000;
    int retryCount = 3;
    bool verifySsl = false;
};

struct DownloadProgress {
    int64_t totalBytes = 0;
    int64_t downloadedBytes = 0;
    double speed = 0.0;
    int activeThreads = 0;
    int completedChunks = 0;
    int totalChunks = 0;
    std::string statusText;
    double percentage = 0.0;
    int64_t etaSeconds = 0;
};

struct DownloadChunk {
    int index = 0;
    int64_t start = 0;
    int64_t end = -1;
    int64_t downloaded = 0;
    std::string tempFilePath;
    bool completed = false;
    bool failed = false;
    std::string error;
};

using ProgressCallback = std::function<void(const DownloadProgress&)>;

class SpeedLimiter {
public:
    explicit SpeedLimiter(int64_t bytesPerSecond = 0);
    void setRate(int64_t bytesPerSecond);
    void wait(int64_t bytes);
    void reset();
private:
    void refill();
    std::mutex m_mutex;
    int64_t m_rate = 0;
    int64_t m_tokens = 0;
    int64_t m_maxTokens = 0;
    std::chrono::steady_clock::time_point m_lastRefill;
};

class MultiThreadDownloader {
public:
    MultiThreadDownloader();
    ~MultiThreadDownloader();

    void setConfig(const DownloadConfig& config);
    DownloadConfig getConfig() const;

    bool download(const std::string& url, const std::string& savePath);

    void pause();
    void resume();
    void cancel();

    DownloadProgress getProgress() const;
    void setProgressCallback(ProgressCallback callback);

    bool isRunning() const;
    bool isPaused() const;
    bool isCancelled() const;

    static std::string extractFilename(const std::string& url);
    static std::wstring toWide(const std::string& str);
    static std::string toNarrow(const std::wstring& wstr);

private:
    bool resolveRedirects(const std::string& url, std::string& finalUrl,
                          int64_t& fileSize, bool& supportsRange);
    void splitChunks(int64_t fileSize);
    bool downloadChunk(DownloadChunk& chunk, const std::string& url);
    bool mergeChunks(const std::string& outputPath);
    void updateProgress();
    void notifyProgress();

    static bool parseUrl(const std::string& url, std::string& scheme,
                         std::string& host, int& port, std::string& path);
    static std::string resolveUrl(const std::string& baseUrl,
                                  const std::string& relativeUrl);

    DownloadConfig m_config;
    DownloadProgress m_progress;
    std::vector<DownloadChunk> m_chunks;
    std::string m_finalUrl;
    int64_t m_fileSize = -1;
    bool m_supportsRange = false;
    std::string m_savePath;

    mutable std::mutex m_progressMutex;
    mutable std::mutex m_pauseMutex;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_cancelled{false};
    std::condition_variable m_pauseCv;

    ProgressCallback m_progressCallback;
    SpeedLimiter m_speedLimiter;

    std::chrono::steady_clock::time_point m_downloadStartTime;
    std::atomic<int64_t> m_totalDownloaded{0};
    std::chrono::steady_clock::time_point m_lastSpeedTime;
    double m_currentSpeed = 0.0;
    std::mutex m_speedMutex;
    int64_t m_lastSpeedDownloaded = 0;
};
