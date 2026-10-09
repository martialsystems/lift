// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "radio/client.h"

#include "pool/pool.h"
#include "radio/stream.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

constexpr uint32_t kRingFrames = 1u << 18;
constexpr uint32_t kRingMask = kRingFrames - 1u;

float g_ring[kRingFrames * 2u];
uint32_t g_write = 0;
uint32_t g_read = 0;
int g_live = 0;
int g_failed = 0;
int g_stop = 0;
float g_keptL = 0.f;
float g_keptR = 0.f;

std::mutex g_mu;
std::condition_variable g_cv;
std::thread g_thread;
bool g_done = true;
std::vector<float> g_sessionL;
std::vector<float> g_sessionR;
char g_name[128];
char g_url[1024];
char g_codec[16];
char g_project[1024];

void ring_push(const float* stereo, int frames) {
    if (stereo == nullptr || frames <= 0) {
        return;
    }
    const uint32_t w = __atomic_load_n(&g_write, __ATOMIC_RELAXED);
    const uint32_t r = __atomic_load_n(&g_read, __ATOMIC_ACQUIRE);
    uint32_t space = kRingFrames - (w - r);
    uint32_t put = static_cast<uint32_t>(frames);
    if (put > space) {
        put = space;
    }
    for (uint32_t i = 0; i < put; ++i) {
        const uint32_t idx = (w + i) & kRingMask;
        g_ring[idx * 2u] = stereo[i * 2u];
        g_ring[idx * 2u + 1u] = stereo[i * 2u + 1u];
    }
    __atomic_store_n(&g_write, w + put, __ATOMIC_RELEASE);
    if (put > 0) {
        __atomic_store_n(&g_live, 1, __ATOMIC_RELEASE);
    }
}

void session_add(const float* stereo, int frames) {
    std::lock_guard<std::mutex> lock(g_mu);
    for (int i = 0; i < frames; ++i) {
        if (static_cast<int>(g_sessionL.size()) >= kCaptureFrames) {
            return;
        }
        g_sessionL.push_back(stereo[i * 2]);
        g_sessionR.push_back(stereo[i * 2 + 1]);
    }
}

void stop_worker() {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
        g_cv.notify_all();
    }
    if (g_thread.joinable()) {
        g_thread.join();
    }
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_sessionL.clear();
        g_sessionR.clear();
        g_done = true;
        g_cv.notify_all();
    }
    __atomic_store_n(&g_stop, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_live, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_failed, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_write, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_read, 0, __ATOMIC_RELAXED);
    g_keptL = 0.f;
    g_keptR = 0.f;
}

bool write_all(int fd, const char* data, int n) {
    int off = 0;
    while (off < n) {
        const ssize_t wrote = ::send(fd, data + off, static_cast<size_t>(n - off), kSendFlags);
        if (wrote < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (wrote == 0) {
            return false;
        }
        off += static_cast<int>(wrote);
    }
    return true;
}

bool parse_url(const char* url, std::string& host, int& port, std::string& path) {
    if (std::strncmp(url, "http://", 7) != 0) {
        return false;
    }
    const char* rest = url + 7;
    const char* slash = std::strchr(rest, '/');
    const std::string hostport = slash != nullptr ? std::string(rest, slash) : std::string(rest);
    path = slash != nullptr ? std::string(slash) : std::string("/");
    if (hostport.empty() || path.empty()) {
        return false;
    }
    const auto colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        int value = 0;
        for (size_t i = colon + 1; i < hostport.size(); ++i) {
            if (hostport[i] < '0' || hostport[i] > '9') {
                return false;
            }
            value = value * 10 + (hostport[i] - '0');
            if (value > 65535) {
                return false;
            }
        }
        if (value <= 0) {
            return false;
        }
        port = value;
    } else {
        host = hostport;
        port = 80;
    }
    return !host.empty();
}

int dial(const std::string& host, int port) {
    char portText[8];
    std::snprintf(portText, sizeof portText, "%d", port);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), portText, &hints, &result) != 0) {
        return -1;
    }
    int fd = -1;
    for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
        fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }
        const int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        const int rc = ::connect(fd, it->ai_addr, it->ai_addrlen);
        if (rc < 0 && errno != EINPROGRESS) {
            ::close(fd);
            fd = -1;
            continue;
        }
        if (rc < 0) {
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            const int waited = ::poll(&pfd, 1, 2000);
            int err = 0;
            socklen_t len = sizeof err;
            if (waited <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
                ::close(fd);
                fd = -1;
                continue;
            }
        }
        fcntl(fd, F_SETFL, flags);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        const int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        break;
    }
    freeaddrinfo(result);
    return fd;
}

bool status_ok(const std::string& head) {
    const auto end = head.find("\r\n");
    const std::string line = end == std::string::npos ? head : head.substr(0, end);
    const bool kind = line.rfind("HTTP/", 0) == 0 || line.rfind("ICY", 0) == 0;
    return kind && line.find(" 200") != std::string::npos;
}

int metaint_of(const std::string& head) {
    std::string lower = head;
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    const char* key = "icy-metaint:";
    const auto at = lower.find(key);
    if (at == std::string::npos) {
        return 0;
    }
    size_t i = at + std::strlen(key);
    while (i < lower.size() && lower[i] == ' ') {
        ++i;
    }
    int value = 0;
    while (i < lower.size() && lower[i] >= '0' && lower[i] <= '9') {
        value = value * 10 + (lower[i] - '0');
        ++i;
        if (value > 1000000) {
            return 0;
        }
    }
    return value;
}

bool read_head(int fd, std::string& head, std::string& extra) {
    std::string acc;
    char buf[1024];
    while (acc.find("\r\n\r\n") == std::string::npos) {
        if (__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE) != 0) {
            return false;
        }
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        acc.append(buf, buf + n);
        if (acc.size() > 16384) {
            return false;
        }
    }
    const auto at = acc.find("\r\n\r\n");
    head = acc.substr(0, at);
    extra = acc.substr(at + 4);
    return status_ok(head);
}

struct Icy {
    int interval = 0;
    int audioLeft = 0;
    int metaLeft = 0;

    void reset(int meta) {
        interval = meta > 0 ? meta : 0;
        audioLeft = interval;
        metaLeft = 0;
    }

    void feed(const unsigned char* in, int n, std::vector<unsigned char>& audio) {
        if (interval <= 0) {
            audio.insert(audio.end(), in, in + n);
            return;
        }
        int i = 0;
        while (i < n) {
            if (metaLeft > 0) {
                const int take = metaLeft < (n - i) ? metaLeft : (n - i);
                i += take;
                metaLeft -= take;
                continue;
            }
            if (audioLeft == 0) {
                metaLeft = static_cast<int>(in[i]) * 16;
                audioLeft = interval;
                ++i;
                continue;
            }
            const int room = n - i;
            const int take = audioLeft < room ? audioLeft : room;
            audio.insert(audio.end(), in + i, in + i + take);
            i += take;
            audioLeft -= take;
        }
    }
};

void drain_audio(StreamDecoder* decoder, const std::vector<unsigned char>& audio) {
    if (audio.empty()) {
        return;
    }
    if (!stream_push(decoder, audio.data(), static_cast<int>(audio.size()))) {
        if (__atomic_load_n(&g_live, __ATOMIC_ACQUIRE) == 0) {
            __atomic_store_n(&g_failed, 1, __ATOMIC_RELEASE);
        }
        __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
        return;
    }
    float block[2048];
    for (;;) {
        const int got = stream_take(decoder, block, 1024);
        if (got <= 0) {
            break;
        }
        ring_push(block, got);
        session_add(block, got);
    }
}

struct NetGuard {
    StreamDecoder* decoder = nullptr;
    int fd = -1;
    ~NetGuard() {
        if (fd >= 0) {
            ::close(fd);
        }
        stream_close(decoder);
        {
            std::lock_guard<std::mutex> lock(g_mu);
            g_done = true;
        }
        g_cv.notify_all();
    }
};

void net_main() {
    NetGuard guard;
    guard.decoder = stream_open(g_codec);
    if (guard.decoder == nullptr) {
        __atomic_store_n(&g_failed, 1, __ATOMIC_RELEASE);
        return;
    }
    std::string host;
    std::string path;
    int port = 80;
    if (!parse_url(g_url, host, port, path)) {
        __atomic_store_n(&g_failed, 1, __ATOMIC_RELEASE);
        return;
    }
    guard.fd = dial(host, port);
    if (guard.fd < 0) {
        __atomic_store_n(&g_failed, 1, __ATOMIC_RELEASE);
        return;
    }
    const std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\nIcy-MetaData: 1\r\nConnection: close\r\n\r\n";
    if (!write_all(guard.fd, req.c_str(), static_cast<int>(req.size()))) {
        __atomic_store_n(&g_failed, 1, __ATOMIC_RELEASE);
        return;
    }
    std::string head;
    std::string extra;
    if (!read_head(guard.fd, head, extra)) {
        __atomic_store_n(&g_failed, 1, __ATOMIC_RELEASE);
        return;
    }
    Icy icy;
    icy.reset(metaint_of(head));
    std::vector<unsigned char> audio;
    audio.reserve(4096);
    auto take_body = [&](const unsigned char* bytes, int n) {
        audio.clear();
        icy.feed(bytes, n, audio);
        drain_audio(guard.decoder, audio);
    };
    if (!extra.empty()) {
        take_body(reinterpret_cast<const unsigned char*>(extra.data()), static_cast<int>(extra.size()));
    }
    unsigned char buf[4096];
    while (__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE) == 0) {
        const ssize_t n = ::recv(guard.fd, buf, sizeof buf, 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }
        take_body(buf, static_cast<int>(n));
    }
    if (__atomic_load_n(&g_live, __ATOMIC_ACQUIRE) == 0) {
        __atomic_store_n(&g_failed, 1, __ATOMIC_RELEASE);
    }
}

bool copy_text(char* dst, int n, const char* src) {
    if (src == nullptr) {
        src = "";
    }
    if (static_cast<int>(std::strlen(src)) >= n) {
        return false;
    }
    std::snprintf(dst, static_cast<size_t>(n), "%s", src);
    return true;
}

bool quoted_field(const std::string& text, const char* key, char* dst, int n) {
    const std::string pattern = std::string("\"") + key + "\"";
    const auto at = text.find(pattern);
    if (at == std::string::npos) {
        return false;
    }
    const auto colon = text.find(':', at + pattern.size());
    if (colon == std::string::npos) {
        return false;
    }
    const auto open = text.find('"', colon + 1);
    if (open == std::string::npos) {
        return false;
    }
    const auto close = text.find('"', open + 1);
    if (close == std::string::npos || close - open >= static_cast<size_t>(n)) {
        return false;
    }
    std::snprintf(dst, static_cast<size_t>(n), "%s", text.substr(open + 1, close - open - 1).c_str());
    return true;
}

}  // namespace

int lift_radio_tune(const RadioRequest* request) {
    if (request == nullptr || request->url == nullptr || request->url[0] == '\0') {
        stop_worker();
        return 1;
    }
    if (std::strncmp(request->url, "http://", 7) != 0 || std::strncmp(request->url, "https://", 8) == 0) {
        stop_worker();
        return 1;
    }
    const char* codec = request->codec != nullptr ? request->codec : "";
    if (std::strcmp(codec, "mp3") != 0 && std::strcmp(codec, "opus") != 0 && std::strcmp(codec, "vorbis") != 0) {
        stop_worker();
        return 1;
    }
    char name[128];
    char url[1024];
    char codecCopy[16];
    char project[1024];
    if (!copy_text(name, 128, request->name) || !copy_text(url, 1024, request->url) ||
        !copy_text(codecCopy, 16, codec) || !copy_text(project, 1024, request->project)) {
        stop_worker();
        return 1;
    }
    stop_worker();
    std::snprintf(g_name, sizeof g_name, "%s", name);
    std::snprintf(g_url, sizeof g_url, "%s", url);
    std::snprintf(g_codec, sizeof g_codec, "%s", codecCopy);
    std::snprintf(g_project, sizeof g_project, "%s", project);
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_done = false;
        g_sessionL.clear();
        g_sessionR.clear();
        g_sessionL.reserve(static_cast<size_t>(kCaptureFrames));
        g_sessionR.reserve(static_cast<size_t>(kCaptureFrames));
    }
    g_thread = std::thread(net_main);
    return 0;
}

int lift_radio_read(float* dst, int n) noexcept {
    if (dst == nullptr || n <= 0 || n > (1 << 26)) {
        return 1;
    }
    const int live = __atomic_load_n(&g_live, __ATOMIC_ACQUIRE);
    const int failed = __atomic_load_n(&g_failed, __ATOMIC_ACQUIRE);
    if (live == 0 || failed != 0) {
        const int samples = n * 2;
        for (int i = 0; i < samples; ++i) {
            dst[i] = 0.f;
        }
        return 1;
    }
    const uint32_t need = static_cast<uint32_t>(n);
    const uint32_t r = __atomic_load_n(&g_read, __ATOMIC_RELAXED);
    const uint32_t w = __atomic_load_n(&g_write, __ATOMIC_ACQUIRE);
    const uint32_t avail = w - r;
    const uint32_t take = avail < need ? avail : need;
    for (uint32_t i = 0; i < take; ++i) {
        const uint32_t idx = (r + i) & kRingMask;
        const float left = g_ring[idx * 2u];
        const float right = g_ring[idx * 2u + 1u];
        dst[i * 2u] = left;
        dst[i * 2u + 1u] = right;
        g_keptL = left;
        g_keptR = right;
    }
    __atomic_store_n(&g_read, r + take, __ATOMIC_RELEASE);
    if (take < need) {
        const float left = g_keptL;
        const float right = g_keptR;
        for (uint32_t i = take; i < need; ++i) {
            dst[i * 2u] = left;
            dst[i * 2u + 1u] = right;
        }
        return 2;
    }
    return 0;
}

int lift_radio_capture(float* dst, int n) {
    std::unique_lock<std::mutex> lock(g_mu);
    g_cv.wait_for(lock, std::chrono::seconds(2), [] {
        return g_done || __atomic_load_n(&g_stop, __ATOMIC_ACQUIRE) != 0;
    });
    const size_t nL = g_sessionL.size();
    const size_t nR = g_sessionR.size();
    const size_t count = nL < nR ? nL : nR;
    std::vector<float> left(g_sessionL.begin(), g_sessionL.begin() + static_cast<std::ptrdiff_t>(count));
    std::vector<float> right(g_sessionR.begin(), g_sessionR.begin() + static_cast<std::ptrdiff_t>(count));
    char project[1024];
    char name[128];
    std::snprintf(project, sizeof project, "%s", g_project);
    std::snprintf(name, sizeof name, "%s", g_name);
    lock.unlock();
    const int frames = static_cast<int>(count);
    if (frames <= 0 || project[0] == '\0') {
        return 1;
    }
    if (dst != nullptr && n > 0) {
        const int m = n < frames ? n : frames;
        for (int i = 0; i < m; ++i) {
            dst[i * 2] = left[static_cast<size_t>(i)];
            dst[i * 2 + 1] = right[static_cast<size_t>(i)];
        }
    }
    char reason[128];
    PoolItem item{};
    const char* label = name[0] != '\0' ? name : "capture";
    const PoolStatus status = pool_add_pcm(project, label, left.data(), right.data(), frames, &item, reason, 128);
    if (status != PoolStatus::Ok && status != PoolStatus::Warn && status != PoolStatus::Duplicate) {
        return 1;
    }
    if (frames >= kCaptureFrames) {
        return 0;
    }
    return 2;
}

int radio_load_directory(const char* path, RadioEntry* entries, int cap) {
    if (path == nullptr || cap < 0) {
        return -1;
    }
    std::ifstream in(path);
    if (!in) {
        return -1;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\n' || text[i] == '\r' || text[i] == '\t')) {
        ++i;
    }
    if (i >= text.size() || text[i] != '[') {
        return -1;
    }
    ++i;
    int count = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\n' || text[i] == '\r' || text[i] == '\t' || text[i] == ',')) {
            ++i;
        }
        if (i < text.size() && text[i] == ']') {
            return count;
        }
        if (i >= text.size() || text[i] != '{') {
            return -1;
        }
        const auto end = text.find('}', i);
        if (end == std::string::npos) {
            return -1;
        }
        const std::string obj = text.substr(i, end - i + 1);
        RadioEntry entry{};
        if (!quoted_field(obj, "name", entry.name, 128) || !quoted_field(obj, "url", entry.url, 512) ||
            !quoted_field(obj, "codec", entry.codec, 16)) {
            return -1;
        }
        if (entries != nullptr && count < cap) {
            entries[count] = entry;
        }
        ++count;
        i = end + 1;
    }
    return -1;
}
