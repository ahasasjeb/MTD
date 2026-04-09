#include "MultiThreadDownloader.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <fstream>
#include <algorithm>
#include <sstream>

#pragma comment(lib, "winhttp.lib")

namespace {
struct WinHttpGuard {
    HINTERNET h = nullptr;
    WinHttpGuard() = default;
    explicit WinHttpGuard(HINTERNET handle) : h(handle) {}
    ~WinHttpGuard() { if (h) WinHttpCloseHandle(h); }
    WinHttpGuard(const WinHttpGuard&) = delete;
    WinHttpGuard& operator=(const WinHttpGuard&) = delete;
    WinHttpGuard(WinHttpGuard&& o) noexcept : h(o.h) { o.h = nullptr; }
    WinHttpGuard& operator=(WinHttpGuard&& o) noexcept {
        if (h) WinHttpCloseHandle(h);
        h = o.h; o.h = nullptr;
        return *this;
    }
    operator HINTERNET() const { return h; }
    explicit operator bool() const { return h != nullptr; }
};
}

SpeedLimiter::SpeedLimiter(int64_t bytesPerSecond)
    : m_rate(bytesPerSecond), m_tokens(0),
      m_maxTokens(bytesPerSecond > 0 ? bytesPerSecond : 0) {
    m_lastRefill = std::chrono::steady_clock::now();
}

void SpeedLimiter::setRate(int64_t bytesPerSecond) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_rate = bytesPerSecond;
    m_maxTokens = m_rate > 0 ? m_rate : 0;
    if (m_tokens > m_maxTokens) m_tokens = m_maxTokens;
}

void SpeedLimiter::wait(int64_t bytes) {
    if (m_rate <= 0 || bytes <= 0) return;
    while (true) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            refill();
            if (m_tokens >= bytes) {
                m_tokens -= bytes;
                return;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void SpeedLimiter::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_tokens = m_maxTokens;
    m_lastRefill = std::chrono::steady_clock::now();
}

void SpeedLimiter::refill() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        now - m_lastRefill).count();
    if (elapsed >= 10000) {
        int64_t newTokens = m_rate * elapsed / 1000000;
        if (newTokens > 0) {
            m_tokens = (std::min)(m_tokens + newTokens, m_maxTokens);
            m_lastRefill = now;
        }
    }
}

MultiThreadDownloader::MultiThreadDownloader() = default;

MultiThreadDownloader::~MultiThreadDownloader() {
    cancel();
}

void MultiThreadDownloader::setConfig(const DownloadConfig& config) {
    m_config = config;
    m_speedLimiter.setRate(config.speedLimit);
}

DownloadConfig MultiThreadDownloader::getConfig() const {
    return m_config;
}

void MultiThreadDownloader::setProgressCallback(ProgressCallback callback) {
    m_progressCallback = std::move(callback);
}

DownloadProgress MultiThreadDownloader::getProgress() const {
    std::lock_guard<std::mutex> lock(m_progressMutex);
    return m_progress;
}

bool MultiThreadDownloader::isRunning() const { return m_running; }
bool MultiThreadDownloader::isPaused() const { return m_paused; }
bool MultiThreadDownloader::isCancelled() const { return m_cancelled; }

void MultiThreadDownloader::pause() {
    m_paused = true;
    std::lock_guard<std::mutex> lock(m_progressMutex);
    m_progress.statusText = "已暂停";
}

void MultiThreadDownloader::resume() {
    m_paused = false;
    m_pauseCv.notify_all();
    std::lock_guard<std::mutex> lock(m_progressMutex);
    m_progress.statusText = "下载中...";
}

void MultiThreadDownloader::cancel() {
    m_cancelled = true;
    m_paused = false;
    m_pauseCv.notify_all();
}

std::wstring MultiThreadDownloader::toWide(const std::string& str) {
    if (str.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring result(static_cast<size_t>(len) - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &result[0], len);
    return result;
}

std::string MultiThreadDownloader::toNarrow(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1,
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string result(static_cast<size_t>(len) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1,
                        &result[0], len, nullptr, nullptr);
    return result;
}

bool MultiThreadDownloader::parseUrl(const std::string& url,
                                     std::string& scheme, std::string& host,
                                     int& port, std::string& path) {
    size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return false;
    scheme = url.substr(0, schemeEnd);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), ::tolower);

    size_t hostStart = schemeEnd + 3;
    size_t pathStart = url.find('/', hostStart);
    if (pathStart == std::string::npos) {
        host = url.substr(hostStart);
        path = "/";
    } else {
        host = url.substr(hostStart, pathStart - hostStart);
        path = url.substr(pathStart);
    }

    size_t bracketEnd = host.find(']');
    size_t portPos = host.rfind(':');
    if (portPos != std::string::npos &&
        (bracketEnd == std::string::npos || portPos > bracketEnd)) {
        port = std::stoi(host.substr(portPos + 1));
        host = host.substr(0, portPos);
    } else {
        port = (scheme == "https") ? 443 : 80;
    }
    return !host.empty();
}

std::string MultiThreadDownloader::resolveUrl(const std::string& baseUrl,
                                              const std::string& relativeUrl) {
    if (relativeUrl.empty()) return baseUrl;
    if (relativeUrl.find("://") != std::string::npos) return relativeUrl;

    std::string scheme, host, path;
    int port;
    if (!parseUrl(baseUrl, scheme, host, port, path)) return relativeUrl;

    std::string portStr;
    if (!((scheme == "https" && port == 443) || (scheme == "http" && port == 80))) {
        portStr = ":" + std::to_string(port);
    }

    if (relativeUrl[0] == '/') {
        return scheme + "://" + host + portStr + relativeUrl;
    }

    size_t lastSlash = path.rfind('/');
    if (lastSlash != std::string::npos) {
        return scheme + "://" + host + portStr +
               path.substr(0, lastSlash + 1) + relativeUrl;
    }
    return relativeUrl;
}

std::string MultiThreadDownloader::extractFilename(const std::string& url) {
    size_t qpos = url.find('?');
    std::string cleanUrl = (qpos != std::string::npos) ? url.substr(0, qpos) : url;
    size_t lastSlash = cleanUrl.rfind('/');
    if (lastSlash != std::string::npos) {
        std::string filename = cleanUrl.substr(lastSlash + 1);
        if (!filename.empty() && filename.find('.') != std::string::npos)
            return filename;
    }
    return "download";
}

bool MultiThreadDownloader::resolveRedirects(const std::string& url,
                                             std::string& finalUrl,
                                             int64_t& fileSize,
                                             bool& supportsRange) {
    std::string currentUrl = url;
    int redirects = 0;

    while (redirects <= m_config.maxRedirects) {
        if (m_cancelled) return false;

        std::string scheme, host, path;
        int port;
        if (!parseUrl(currentUrl, scheme, host, port, path)) {
            std::lock_guard<std::mutex> lock(m_progressMutex);
            m_progress.statusText = "URL解析失败";
            return false;
        }

        WinHttpGuard hSession(WinHttpOpen(toWide(m_config.userAgent).c_str(),
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0));
        if (!hSession) {
            std::lock_guard<std::mutex> lock(m_progressMutex);
            m_progress.statusText = "创建HTTP会话失败 (错误: " +
                std::to_string(GetLastError()) + ")";
            return false;
        }

        WinHttpSetTimeouts(hSession, m_config.connectTimeout,
                           m_config.connectTimeout,
                           m_config.readTimeout, m_config.readTimeout);

        WinHttpGuard hConnect(WinHttpConnect(hSession, toWide(host).c_str(),
            static_cast<INTERNET_PORT>(port), 0));
        if (!hConnect) {
            std::lock_guard<std::mutex> lock(m_progressMutex);
            m_progress.statusText = "连接服务器失败: " + host;
            return false;
        }

        DWORD flags = (scheme == "https") ? WINHTTP_FLAG_SECURE : 0;
        WinHttpGuard hRequest(WinHttpOpenRequest(hConnect, L"HEAD",
            toWide(path).c_str(), NULL, NULL, NULL, flags));
        if (!hRequest) {
            std::lock_guard<std::mutex> lock(m_progressMutex);
            m_progress.statusText = "创建请求失败";
            return false;
        }

        if (!m_config.verifySsl && scheme == "https") {
            DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS,
                             &secFlags, sizeof(secFlags));
        }

        if (!WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0)) {
            DWORD err = GetLastError();
            if (err == ERROR_WINHTTP_CANNOT_CONNECT || err == ERROR_WINHTTP_TIMEOUT) {
                std::lock_guard<std::mutex> lock(m_progressMutex);
                m_progress.statusText = "连接超时或被拒绝: " + host;
                return false;
            }
            continue;
        }

        if (!WinHttpReceiveResponse(hRequest, NULL)) {
            std::lock_guard<std::mutex> lock(m_progressMutex);
            m_progress.statusText = "接收响应失败 (错误: " +
                std::to_string(GetLastError()) + ")";
            return false;
        }

        DWORD statusCode = 0;
        DWORD size = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            NULL, &statusCode, &size, NULL);

        if (statusCode == 301 || statusCode == 302 || statusCode == 303 ||
            statusCode == 307 || statusCode == 308) {
            WCHAR location[4096] = {};
            DWORD locSize = sizeof(location);
            if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION,
                                    NULL, location, &locSize, NULL)) {
                std::string locationStr = toNarrow(location);
                currentUrl = resolveUrl(currentUrl, locationStr);
                redirects++;
                continue;
            }
            std::lock_guard<std::mutex> lock(m_progressMutex);
            m_progress.statusText = "重定向缺少Location头 (HTTP " +
                std::to_string(statusCode) + ")";
            return false;
        }

        if (statusCode == 405) {
            WinHttpGuard hGetSession(WinHttpOpen(
                toWide(m_config.userAgent).c_str(),
                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0));
            if (!hGetSession) return false;
            WinHttpSetTimeouts(hGetSession, m_config.connectTimeout,
                               m_config.connectTimeout,
                               m_config.readTimeout, m_config.readTimeout);
            WinHttpGuard hGetConnect(WinHttpConnect(hGetSession,
                toWide(host).c_str(),
                static_cast<INTERNET_PORT>(port), 0));
            if (!hGetConnect) return false;
            WinHttpGuard hGetRequest(WinHttpOpenRequest(hGetConnect, L"GET",
                toWide(path).c_str(), NULL, NULL, NULL, flags));
            if (!hGetRequest) return false;

            if (!m_config.verifySsl && scheme == "https") {
                DWORD sf = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                    SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                    SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                    SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
                WinHttpSetOption(hGetRequest, WINHTTP_OPTION_SECURITY_FLAGS,
                                 &sf, sizeof(sf));
            }

            std::wstring rangeHdr = L"Range: bytes=0-0\r\n";
            if (!WinHttpSendRequest(hGetRequest, rangeHdr.c_str(), -1,
                                    NULL, 0, 0, 0)) return false;
            if (!WinHttpReceiveResponse(hGetRequest, NULL)) return false;

            DWORD getStatus = 0;
            DWORD gs = sizeof(getStatus);
            WinHttpQueryHeaders(hGetRequest,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                NULL, &getStatus, &gs, NULL);

            if (getStatus == 206) {
                supportsRange = true;
                WCHAR cr[256] = {};
                DWORD crSize = sizeof(cr);
                if (WinHttpQueryHeaders(hGetRequest, WINHTTP_QUERY_CONTENT_RANGE,
                                        NULL, cr, &crSize, NULL)) {
                    std::string crStr = toNarrow(cr);
                    size_t slash = crStr.rfind('/');
                    if (slash != std::string::npos) {
                        fileSize = std::stoll(crStr.substr(slash + 1));
                    }
                }
            } else {
                supportsRange = false;
                WCHAR cl[64] = {};
                DWORD clSz = sizeof(cl);
                if (WinHttpQueryHeaders(hGetRequest, WINHTTP_QUERY_CONTENT_LENGTH,
                                        NULL, cl, &clSz, NULL)) {
                    fileSize = _wtoi64(cl);
                }
            }

            finalUrl = currentUrl;
            return true;
        }

        if (statusCode >= 400) {
            std::lock_guard<std::mutex> lock(m_progressMutex);
            m_progress.statusText = "HTTP错误: " + std::to_string(statusCode);
            return false;
        }

        fileSize = -1;
        WCHAR contentLength[64] = {};
        DWORD clSize = sizeof(contentLength);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH,
                                NULL, contentLength, &clSize, NULL)) {
            fileSize = _wtoi64(contentLength);
        }

        supportsRange = false;
        WCHAR acceptRanges[64] = {};
        DWORD arSize = sizeof(acceptRanges);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_ACCEPT_RANGES,
                                NULL, acceptRanges, &arSize, NULL)) {
            std::string ar = toNarrow(acceptRanges);
            supportsRange = (ar.find("bytes") != std::string::npos);
        }

        if (!supportsRange || fileSize <= 0) {
            WinHttpGuard hTestSession(WinHttpOpen(
                toWide(m_config.userAgent).c_str(),
                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0));
            if (hTestSession) {
                WinHttpSetTimeouts(hTestSession, m_config.connectTimeout,
                    m_config.connectTimeout, m_config.readTimeout, m_config.readTimeout);
                WinHttpGuard hTestConnect(WinHttpConnect(hTestSession,
                    toWide(host).c_str(),
                    static_cast<INTERNET_PORT>(port), 0));
                if (hTestConnect) {
                    WinHttpGuard hTestRequest(WinHttpOpenRequest(hTestConnect,
                        L"GET", toWide(path).c_str(), NULL, NULL, NULL, flags));
                    if (hTestRequest) {
                        if (!m_config.verifySsl && scheme == "https") {
                            DWORD sf = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                                SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                                SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                                SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
                            WinHttpSetOption(hTestRequest,
                                WINHTTP_OPTION_SECURITY_FLAGS, &sf, sizeof(sf));
                        }
                        std::wstring rh = L"Range: bytes=0-0\r\n";
                        if (WinHttpSendRequest(hTestRequest, rh.c_str(), -1,
                                               NULL, 0, 0, 0) &&
                            WinHttpReceiveResponse(hTestRequest, NULL)) {
                            DWORD sc = 0;
                            DWORD ss = sizeof(sc);
                            WinHttpQueryHeaders(hTestRequest,
                                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                NULL, &sc, &ss, NULL);
                            if (sc == 206) {
                                supportsRange = true;
                                WCHAR cr[256] = {};
                                DWORD crSize = sizeof(cr);
                                if (WinHttpQueryHeaders(hTestRequest,
                                        WINHTTP_QUERY_CONTENT_RANGE,
                                        NULL, cr, &crSize, NULL)) {
                                    std::string crStr = toNarrow(cr);
                                    size_t slash = crStr.rfind('/');
                                    if (slash != std::string::npos) {
                                        int64_t parsed = _strtoi64(
                                            crStr.substr(slash + 1).c_str(),
                                            nullptr, 10);
                                        if (parsed > 0) fileSize = parsed;
                                    }
                                }
                            } else {
                                supportsRange = false;
                                if (fileSize <= 0) {
                                    WCHAR cl[64] = {};
                                    DWORD clSize2 = sizeof(cl);
                                    if (WinHttpQueryHeaders(hTestRequest,
                                            WINHTTP_QUERY_CONTENT_LENGTH,
                                            NULL, cl, &clSize2, NULL)) {
                                        int64_t parsed = _wtoi64(cl);
                                        if (parsed > 0) fileSize = parsed;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        finalUrl = currentUrl;
        return true;
    }

    std::lock_guard<std::mutex> lock(m_progressMutex);
    m_progress.statusText = "重定向次数超过限制 (" +
        std::to_string(m_config.maxRedirects) + ")";
    return false;
}

void MultiThreadDownloader::splitChunks(int64_t fileSize) {
    const int64_t minChunkSize = 256 * 1024;
    int64_t maxChunks = (std::max)(int64_t(1), fileSize / minChunkSize);
    int chunkCount = static_cast<int>(
        (std::min)(static_cast<int64_t>(m_config.threadCount), maxChunks));
    int64_t chunkSize = fileSize / chunkCount;

    m_chunks.clear();
    for (int i = 0; i < chunkCount; i++) {
        DownloadChunk chunk;
        chunk.index = i;
        chunk.start = static_cast<int64_t>(i) * chunkSize;
        chunk.end = (i == chunkCount - 1)
            ? fileSize - 1
            : static_cast<int64_t>(i + 1) * chunkSize - 1;
        chunk.downloaded = 0;
        chunk.tempFilePath = m_savePath + ".part_" + std::to_string(i) + ".tmp";
        chunk.completed = false;
        chunk.failed = false;
        m_chunks.push_back(chunk);
    }
}

bool MultiThreadDownloader::downloadChunk(DownloadChunk& chunk,
                                          const std::string& url) {
    for (int retry = 0; retry <= m_config.retryCount; retry++) {
        if (m_cancelled) { chunk.failed = true; return false; }

        chunk.downloaded = 0;
        chunk.error.clear();
        chunk.failed = false;

        std::string scheme, host, path;
        int port;
        if (!parseUrl(url, scheme, host, port, path)) {
            chunk.error = "URL解析失败";
            chunk.failed = true;
            continue;
        }

        WinHttpGuard hSession(WinHttpOpen(toWide(m_config.userAgent).c_str(),
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0));
        if (!hSession) {
            chunk.error = "创建HTTP会话失败";
            chunk.failed = true;
            continue;
        }

        WinHttpSetTimeouts(hSession, m_config.connectTimeout,
                           m_config.connectTimeout,
                           m_config.readTimeout, m_config.readTimeout);

        WinHttpGuard hConnect(WinHttpConnect(hSession, toWide(host).c_str(),
            static_cast<INTERNET_PORT>(port), 0));
        if (!hConnect) {
            chunk.error = "连接服务器失败: " + host;
            chunk.failed = true;
            continue;
        }

        DWORD flags = (scheme == "https") ? WINHTTP_FLAG_SECURE : 0;
        WinHttpGuard hRequest(WinHttpOpenRequest(hConnect, L"GET",
            toWide(path).c_str(), NULL, NULL, NULL, flags));
        if (!hRequest) {
            chunk.error = "创建请求失败";
            chunk.failed = true;
            continue;
        }

        if (!m_config.verifySsl && scheme == "https") {
            DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS,
                             &secFlags, sizeof(secFlags));
        }

        std::wstring headers;
        if (m_supportsRange && (chunk.start > 0 || chunk.end > 0)) {
            headers = L"Range: bytes=" + std::to_wstring(chunk.start) +
                      L"-" + std::to_wstring(chunk.end) + L"\r\n";
        }

        if (!WinHttpSendRequest(hRequest,
                                headers.empty() ? NULL : headers.c_str(),
                                headers.empty() ? 0 : -1,
                                NULL, 0, 0, 0)) {
            chunk.error = "发送请求失败 (错误: " +
                std::to_string(GetLastError()) + ")";
            chunk.failed = true;
            continue;
        }

        if (!WinHttpReceiveResponse(hRequest, NULL)) {
            chunk.error = "接收响应失败 (错误: " +
                std::to_string(GetLastError()) + ")";
            chunk.failed = true;
            continue;
        }

        DWORD statusCode = 0;
        DWORD size = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            NULL, &statusCode, &size, NULL);

        if (statusCode == 301 || statusCode == 302 || statusCode == 303 ||
            statusCode == 307 || statusCode == 308) {
            WCHAR location[4096] = {};
            DWORD locSize = sizeof(location);
            if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION,
                                    NULL, location, &locSize, NULL)) {
                std::string newUrl = resolveUrl(url, toNarrow(location));
                hRequest = WinHttpGuard();
                hConnect = WinHttpGuard();
                hSession = WinHttpGuard();

                DownloadChunk retryChunk = chunk;
                retryChunk.tempFilePath = chunk.tempFilePath + "_r" +
                    std::to_string(retry);
                bool result = downloadChunk(retryChunk, newUrl);
                if (result) {
                    if (retryChunk.tempFilePath != chunk.tempFilePath) {
                        MoveFileExW(
                            toWide(retryChunk.tempFilePath).c_str(),
                            toWide(chunk.tempFilePath).c_str(),
                            MOVEFILE_REPLACE_EXISTING);
                    }
                    chunk.completed = true;
                    chunk.downloaded = retryChunk.downloaded;
                    return true;
                }
                chunk.error = retryChunk.error;
                chunk.failed = true;
                continue;
            }
            chunk.error = "重定向缺少Location头";
            chunk.failed = true;
            continue;
        }

        if (statusCode != 200 && statusCode != 206) {
            chunk.error = "HTTP错误: " + std::to_string(statusCode);
            chunk.failed = true;
            continue;
        }

        if (statusCode == 200 && chunk.start > 0 && m_supportsRange) {
            chunk.error = "服务器不支持Range请求";
            chunk.failed = true;
            continue;
        }

        FILE* outFile = nullptr;
        if (_wfopen_s(&outFile, toWide(chunk.tempFilePath).c_str(), L"wb") != 0 ||
            !outFile) {
            chunk.error = "创建临时文件失败: " + chunk.tempFilePath;
            chunk.failed = true;
            continue;
        }

        setvbuf(outFile, nullptr, _IOFBF, 1024 * 1024);

        std::vector<char> buffer(1024 * 1024);
        bool readError = false;

        while (true) {
            if (m_cancelled) break;

            while (m_paused && !m_cancelled) {
                std::unique_lock<std::mutex> lock(m_pauseMutex);
                m_pauseCv.wait_for(lock, std::chrono::milliseconds(100));
            }
            if (m_cancelled) break;

            DWORD bytesRead = 0;
            if (!WinHttpReadData(hRequest, buffer.data(),
                                 static_cast<DWORD>(buffer.size()),
                                 &bytesRead)) {
                readError = true;
                break;
            }
            if (bytesRead == 0) {
                break;
            }

            if (fwrite(buffer.data(), 1, bytesRead, outFile) != bytesRead) {
                chunk.error = "写入文件失败";
                readError = true;
                break;
            }

            chunk.downloaded += bytesRead;
            m_totalDownloaded.fetch_add(bytesRead, std::memory_order_relaxed);
            m_speedLimiter.wait(bytesRead);
            updateProgress();
        }

        fclose(outFile);

        if (m_cancelled) {
            chunk.failed = true;
            DeleteFileW(toWide(chunk.tempFilePath).c_str());
            return false;
        }

        if (readError) {
            chunk.error = "读取数据失败";
            chunk.failed = true;
            continue;
        }

        int64_t expectedSize = chunk.end - chunk.start + 1;
        if (expectedSize > 0 && chunk.downloaded < expectedSize) {
            chunk.error = "数据不完整: " + std::to_string(chunk.downloaded) +
                "/" + std::to_string(expectedSize);
            chunk.failed = true;
            continue;
        }

        chunk.completed = true;
        return true;
    }

    chunk.failed = true;
    return false;
}

bool MultiThreadDownloader::mergeChunks(const std::string& outputPath) {
    FILE* outFile = nullptr;
    if (_wfopen_s(&outFile, toWide(outputPath).c_str(), L"wb") != 0 || !outFile) return false;

    std::vector<char> buffer(1048576);

    for (const auto& chunk : m_chunks) {
        FILE* inFile = nullptr;
        if (_wfopen_s(&inFile, toWide(chunk.tempFilePath).c_str(), L"rb") != 0 ||
            !inFile) {
            fclose(outFile);
            DeleteFileW(toWide(outputPath).c_str());
            return false;
        }

        size_t bytesRead;
        while ((bytesRead = fread(buffer.data(), 1, buffer.size(), inFile)) > 0) {
            fwrite(buffer.data(), 1, bytesRead, outFile);
        }

        fclose(inFile);
    }

    fclose(outFile);
    return true;
}

void MultiThreadDownloader::updateProgress() {
    std::lock_guard<std::mutex> lock(m_progressMutex);

    int64_t total = 0;
    int completed = 0;
    int active = 0;
    for (const auto& chunk : m_chunks) {
        total += chunk.downloaded;
        if (chunk.completed) completed++;
        if (!chunk.completed && !chunk.failed && chunk.downloaded > 0) active++;
    }
    m_progress.downloadedBytes = total;
    m_progress.completedChunks = completed;
    m_progress.activeThreads = active;

    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> speedLock(m_speedMutex);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_lastSpeedTime).count();
        if (elapsed >= 500) {
            int64_t bytesDiff = total - m_lastSpeedDownloaded;
            double instantSpeed = static_cast<double>(bytesDiff) /
                                  (elapsed / 1000.0);
            m_currentSpeed = (m_currentSpeed < 0)
                ? instantSpeed
                : m_currentSpeed * 0.7 + instantSpeed * 0.3;
            m_lastSpeedDownloaded = total;
            m_lastSpeedTime = now;
        }
    }
    m_progress.speed = (std::max)(0.0, m_currentSpeed);

    if (m_progress.totalBytes > 0) {
        m_progress.percentage = static_cast<double>(total) /
                                m_progress.totalBytes * 100.0;
        if (m_currentSpeed > 0) {
            m_progress.etaSeconds = static_cast<int64_t>(
                (m_progress.totalBytes - total) / m_currentSpeed);
        }
    }
}

void MultiThreadDownloader::notifyProgress() {
    if (m_progressCallback) {
        std::lock_guard<std::mutex> lock(m_progressMutex);
        m_progressCallback(m_progress);
    }
}

bool MultiThreadDownloader::download(const std::string& url,
                                     const std::string& savePath) {
    m_savePath = savePath;
    m_running = true;
    m_paused = false;
    m_cancelled = false;
    m_finalUrl.clear();
    m_chunks.clear();
    m_fileSize = -1;
    m_supportsRange = false;
    m_totalDownloaded = 0;
    m_currentSpeed = -1.0;
    m_lastSpeedDownloaded = 0;

    {
        std::lock_guard<std::mutex> lock(m_progressMutex);
        m_progress = DownloadProgress();
        m_progress.statusText = "解析重定向...";
    }
    m_speedLimiter.setRate(m_config.speedLimit);
    m_speedLimiter.reset();
    m_downloadStartTime = std::chrono::steady_clock::now();
    m_lastSpeedTime = m_downloadStartTime;
    notifyProgress();

    if (!resolveRedirects(url, m_finalUrl, m_fileSize, m_supportsRange)) {
        if (!m_cancelled) {
            std::lock_guard<std::mutex> lock(m_progressMutex);
            if (m_progress.statusText.empty())
                m_progress.statusText = "解析重定向失败";
            notifyProgress();
        }
        m_running = false;
        return false;
    }

    if (m_cancelled) { m_running = false; return false; }

    {
        std::lock_guard<std::mutex> lock(m_progressMutex);
        m_progress.totalBytes = m_fileSize > 0 ? m_fileSize : 0;
        m_progress.statusText = m_supportsRange
            ? "多线程下载中..."
            : "单线程下载中 (服务器不支持Range)...";
    }
    notifyProgress();

    if (m_fileSize <= 0 || !m_supportsRange) {
        DownloadChunk chunk;
        chunk.index = 0;
        chunk.start = 0;
        chunk.end = m_fileSize > 0 ? m_fileSize - 1 : -1;
        chunk.downloaded = 0;
        chunk.tempFilePath = savePath + ".part_0.tmp";
        chunk.completed = false;
        chunk.failed = false;
        m_chunks.push_back(chunk);
        {
            std::lock_guard<std::mutex> lock(m_progressMutex);
            m_progress.totalChunks = 1;
        }

        if (!downloadChunk(m_chunks[0], m_finalUrl)) {
            std::lock_guard<std::mutex> lock(m_progressMutex);
            m_progress.statusText = "下载失败: " + m_chunks[0].error;
            notifyProgress();
            m_running = false;
            return false;
        }
    } else {
        splitChunks(m_fileSize);
        {
            std::lock_guard<std::mutex> lock(m_progressMutex);
            m_progress.totalChunks = static_cast<int>(m_chunks.size());
        }
        notifyProgress();

        std::vector<std::thread> threads;
        for (size_t i = 0; i < m_chunks.size(); i++) {
            threads.emplace_back([this, i]() {
                downloadChunk(m_chunks[i], m_finalUrl);
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        for (const auto& chunk : m_chunks) {
            if (!chunk.completed) {
                std::lock_guard<std::mutex> lock(m_progressMutex);
                m_progress.statusText = "下载失败: " + chunk.error;
                notifyProgress();
                m_running = false;
                return false;
            }
        }
    }

    if (m_cancelled) { m_running = false; return false; }

    {
        std::lock_guard<std::mutex> lock(m_progressMutex);
        m_progress.statusText = "合并文件...";
    }
    notifyProgress();

    if (!mergeChunks(savePath)) {
        std::lock_guard<std::mutex> lock(m_progressMutex);
        m_progress.statusText = "文件合并失败";
        notifyProgress();
        m_running = false;
        return false;
    }

    for (const auto& chunk : m_chunks) {
        DeleteFileW(toWide(chunk.tempFilePath).c_str());
    }

    {
        std::lock_guard<std::mutex> lock(m_progressMutex);
        m_progress.statusText = "下载完成";
        m_progress.percentage = 100.0;
        m_progress.downloadedBytes = m_progress.totalBytes > 0
            ? m_progress.totalBytes
            : m_progress.downloadedBytes;
        m_progress.speed = 0;
        m_progress.etaSeconds = 0;
    }
    notifyProgress();
    m_running = false;
    return true;
}
