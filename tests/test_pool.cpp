// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "audio/prepare.h"
#include "audio/process_block.h"
#include "pool/pool.h"
#include "project/project.h"
#include "radio/client.h"
#include "radio/stream.h"
#include "tape/transport.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_fails = 0;

void check(bool ok, const char* file, int line, const char* text) {
    if (!ok) {
        std::cerr << "FAIL " << file << ":" << line << " " << text << "\n";
        ++g_fails;
    }
}

#define CHECK(cond) check(static_cast<bool>(cond), __FILE__, __LINE__, #cond)

void le16(std::vector<unsigned char>& b, uint16_t v) {
    b.push_back(static_cast<unsigned char>(v & 255u));
    b.push_back(static_cast<unsigned char>((v >> 8) & 255u));
}

void le32(std::vector<unsigned char>& b, uint32_t v) {
    b.push_back(static_cast<unsigned char>(v & 255u));
    b.push_back(static_cast<unsigned char>((v >> 8) & 255u));
    b.push_back(static_cast<unsigned char>((v >> 16) & 255u));
    b.push_back(static_cast<unsigned char>((v >> 24) & 255u));
}

void be16(std::vector<unsigned char>& b, uint16_t v) {
    b.push_back(static_cast<unsigned char>((v >> 8) & 255u));
    b.push_back(static_cast<unsigned char>(v & 255u));
}

void be32(std::vector<unsigned char>& b, uint32_t v) {
    b.push_back(static_cast<unsigned char>((v >> 24) & 255u));
    b.push_back(static_cast<unsigned char>((v >> 16) & 255u));
    b.push_back(static_cast<unsigned char>((v >> 8) & 255u));
    b.push_back(static_cast<unsigned char>(v & 255u));
}

void write_file(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void write_wav(
    const std::filesystem::path& path,
    int rate,
    int channels,
    int frames,
    int16_t left,
    int16_t right,
    int lieFrames) {
    const int declared = lieFrames > 0 ? lieFrames : frames;
    const uint32_t dataBytes = static_cast<uint32_t>(declared) * static_cast<uint32_t>(channels) * 2u;
    std::vector<unsigned char> b;
    b.insert(b.end(), {'R', 'I', 'F', 'F'});
    le32(b, 36u + (lieFrames > 0 ? 0u : dataBytes));
    b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    le32(b, 16);
    le16(b, 1);
    le16(b, static_cast<uint16_t>(channels));
    le32(b, static_cast<uint32_t>(rate));
    le32(b, static_cast<uint32_t>(rate) * static_cast<uint32_t>(channels) * 2u);
    le16(b, static_cast<uint16_t>(channels * 2));
    le16(b, 16);
    b.insert(b.end(), {'d', 'a', 't', 'a'});
    le32(b, dataBytes);
    if (lieFrames <= 0) {
        for (int i = 0; i < frames; ++i) {
            le16(b, static_cast<uint16_t>(left));
            if (channels == 2) {
                le16(b, static_cast<uint16_t>(right));
            }
        }
    }
    write_file(path, b);
}

void write_aiff(const std::filesystem::path& path) {
    const int frames = 4;
    const int channels = 2;
    const uint32_t nbytes = static_cast<uint32_t>(frames * channels * 2);
    std::vector<unsigned char> b;
    b.insert(b.end(), {'F', 'O', 'R', 'M'});
    be32(b, 46u + nbytes);
    b.insert(b.end(), {'A', 'I', 'F', 'F', 'C', 'O', 'M', 'M'});
    be32(b, 18);
    be16(b, static_cast<uint16_t>(channels));
    be32(b, static_cast<uint32_t>(frames));
    be16(b, 16);
    const unsigned char rate[10] = {0x40, 0x0e, 0xbb, 0x80, 0, 0, 0, 0, 0, 0};
    b.insert(b.end(), rate, rate + 10);
    b.insert(b.end(), {'S', 'S', 'N', 'D'});
    be32(b, 8u + nbytes);
    be32(b, 0);
    be32(b, 0);
    for (int i = 0; i < frames; ++i) {
        be16(b, 16384);
        be16(b, static_cast<uint16_t>(-16384));
    }
    write_file(path, b);
}

std::vector<unsigned char> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct Tmp {
    std::filesystem::path path;
    explicit Tmp(const char* name) {
        path = std::filesystem::temp_directory_path() / (std::string(name) + "-" + std::to_string(::getpid()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~Tmp() { std::filesystem::remove_all(path); }
};

struct QuietRadio {
    ~QuietRadio() {
        RadioRequest request{};
        request.url = "";
        lift_radio_tune(&request);
    }
};

int decode_fixture(const char* codec, const std::filesystem::path& path) {
    const std::vector<unsigned char> bytes = read_bytes(path);
    StreamDecoder* decoder = stream_open(codec);
    if (decoder == nullptr) {
        return -1;
    }
    int total = 0;
    size_t off = 0;
    while (off < bytes.size()) {
        const size_t n = bytes.size() - off > 512 ? 512 : bytes.size() - off;
        if (!stream_push(decoder, bytes.data() + off, static_cast<int>(n))) {
            break;
        }
        off += n;
        float block[2048];
        for (;;) {
            const int got = stream_take(decoder, block, 1024);
            if (got <= 0) {
                break;
            }
            total += got;
        }
    }
    stream_close(decoder);
    return total;
}

void test_units() {
    char hex[65];
    pool_hash_bytes("", 0, hex);
    CHECK(std::strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    pool_hash_bytes("abc", 3, hex);
    CHECK(std::strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);

    CHECK(pool_admit(0, 1) == PoolStatus::Ok);
    CHECK(pool_admit(kPoolWarnBytes - 1, 0) == PoolStatus::Ok);
    CHECK(pool_admit(kPoolWarnBytes - 1, 1) == PoolStatus::Warn);
    CHECK(pool_admit(kPoolWarnBytes, 0) == PoolStatus::Warn);
    CHECK(pool_admit(kPoolCapBytes - 8, 8) == PoolStatus::Warn);
    CHECK(pool_admit(kPoolCapBytes - 8, 9) == PoolStatus::OverCap);
    CHECK(pool_admit(kPoolCapBytes, 0) == PoolStatus::OverCap);

    int start = 0;
    int length = 0;
    pool_slice(100, 24, 0, start, length);
    CHECK(start == 0);
    CHECK(length == 4);
    pool_slice(100, 24, 23, start, length);
    CHECK(start == 95);
    CHECK(length == 5);
    pool_slice(24, 24, 5, start, length);
    CHECK(start == 5);
    CHECK(length == 1);
    CHECK(std::fabs(pool_pitch_ratio(60, 60) - 1.f) < 1e-6f);
    CHECK(std::fabs(pool_pitch_ratio(60, 72) - 2.f) < 1e-5f);
    CHECK(std::fabs(pool_pitch_ratio(60, 48) - 0.5f) < 1e-5f);
}

bool import_ok(const char* project, const char* path, PoolItem& item, char* reason) {
    const PoolStatus status = pool_import(project, path, &item, reason, 128);
    if (status != PoolStatus::Ok && status != PoolStatus::Warn) {
        std::cerr << "import " << path << " -> " << reason << "\n";
        return false;
    }
    return true;
}

void test_pool() {
    Tmp tmp("lift-pool");
    const std::string dir = tmp.path.string();
    PoolItem scratch[8];
    CHECK(pool_load(dir.c_str(), scratch, 8) == 0);

    write_wav(tmp.path / "one.wav", 48000, 2, 32, 16384, -16384, 0);
    char reason[128] = {};
    PoolItem first{};
    CHECK(import_ok(dir.c_str(), (tmp.path / "one.wav").string().c_str(), first, reason));
    CHECK(first.frames == 32);
    CHECK(first.rate == kSampleRate);
    CHECK(std::strcmp(first.map, "slices") == 0);
    CHECK(first.root == 60);
    PoolItem again{};
    const PoolStatus dup = pool_import(dir.c_str(), (tmp.path / "one.wav").string().c_str(), &again, reason, 128);
    CHECK(dup == PoolStatus::Duplicate);
    CHECK(std::strcmp(reason, "already in the pool") == 0);
    CHECK(std::strcmp(again.hash, first.hash) == 0);
    CHECK(pool_load(dir.c_str(), scratch, 8) == 1);

    float left[32];
    float right[32];
    CHECK(pool_read(dir.c_str(), first.hash, left, right, 32) == PoolStatus::Ok);
    CHECK(std::fabs(left[0] - 0.5f) < 1e-4f);
    CHECK(std::fabs(right[0] + 0.5f) < 1e-4f);

    write_wav(tmp.path / "slow.wav", 44100, 2, 441, 16000, 16000, 0);
    PoolItem slow{};
    CHECK(import_ok(dir.c_str(), (tmp.path / "slow.wav").string().c_str(), slow, reason));
    CHECK(slow.frames == 480);
    CHECK(slow.rate == kSampleRate);

    write_wav(tmp.path / "mono.wav", 48000, 1, 4, 16384, 0, 0);
    PoolItem mono{};
    CHECK(import_ok(dir.c_str(), (tmp.path / "mono.wav").string().c_str(), mono, reason));
    CHECK(mono.frames == 4);
    float monoL[4];
    float monoR[4];
    CHECK(pool_read(dir.c_str(), mono.hash, monoL, monoR, 4) == PoolStatus::Ok);
    CHECK(std::fabs(monoL[0] - monoR[0]) < 1e-6f);
    CHECK(std::fabs(monoL[0] - 0.5f) < 1e-4f);

    write_aiff(tmp.path / "four.aif");
    PoolItem aiff{};
    CHECK(import_ok(dir.c_str(), (tmp.path / "four.aif").string().c_str(), aiff, reason));
    CHECK(aiff.frames == 4);
    CHECK(aiff.rate == kSampleRate);

    const std::filesystem::path root{LIFT_SOURCE_DIR};
    PoolItem flac{};
    CHECK(import_ok(dir.c_str(), (root / "tests/fixtures/tone.flac").string().c_str(), flac, reason));
    CHECK(flac.frames > 1000);
    CHECK(flac.rate == kSampleRate);
    PoolItem mp3{};
    CHECK(import_ok(dir.c_str(), (root / "tests/fixtures/tone.mp3").string().c_str(), mp3, reason));
    CHECK(mp3.frames > 20000);
    CHECK(mp3.rate == kSampleRate);
    {
        std::vector<float> toneL(4096);
        std::vector<float> toneR(4096);
        CHECK(pool_read(dir.c_str(), mp3.hash, toneL.data(), toneR.data(), 4096) == PoolStatus::Ok);
        float tonePeak = 0.f;
        for (float sample : toneL) {
            const float a = std::fabs(sample);
            if (a > tonePeak) {
                tonePeak = a;
            }
        }
        CHECK(tonePeak > 0.05f);
    }

    const int longFrames = 48000 * 60 * 31;
    write_wav(tmp.path / "long.wav", 48000, 2, 0, 0, 0, longFrames);
    const PoolStatus duration = pool_import(dir.c_str(), (tmp.path / "long.wav").string().c_str(), nullptr, reason, 128);
    CHECK(duration == PoolStatus::Duration);
    CHECK(std::strstr(reason, "30") != nullptr);

    write_file(tmp.path / "note.txt", {'h', 'e', 'l', 'l', 'o'});
    const PoolStatus format = pool_import(dir.c_str(), (tmp.path / "note.txt").string().c_str(), nullptr, reason, 128);
    CHECK(format == PoolStatus::Format);

    CHECK(pool_set_map(dir.c_str(), first.hash, "chromatic", 72) == PoolStatus::Ok);
    CHECK(pool_set_map(dir.c_str(), first.hash, "kit", 60) == PoolStatus::Format);
    const int count = pool_load(dir.c_str(), scratch, 8);
    CHECK(count >= 1);
    bool mapped = false;
    for (int i = 0; i < count && i < 8; ++i) {
        if (std::strcmp(scratch[i].hash, first.hash) == 0) {
            mapped = std::strcmp(scratch[i].map, "chromatic") == 0 && scratch[i].root == 72;
        }
    }
    CHECK(mapped);

    TapeRuntime rt;
    transport_init(rt);
    prepare_tracks(rt, 2048);
    ProjectInfo info;
    project_info_init(info);
    info.frames = 2048;
    info.tempo = 110.f;
    CHECK(project_write(dir.c_str(), info, rt) == LoadStatus::Ok);
    release_tracks(rt);
    const int afterWrite = pool_load(dir.c_str(), scratch, 8);
    CHECK(afterWrite == count);

    Tmp copied("lift-pool-copy");
    std::filesystem::copy(tmp.path, copied.path / "project", std::filesystem::copy_options::recursive);
    const std::string copyDir = (copied.path / "project").string();
    ProjectInfo loaded;
    project_info_init(loaded);
    CHECK(project_read_info(copyDir.c_str(), loaded) == LoadStatus::Ok);
    CHECK(loaded.tempo == 110.f);
    PoolItem reloaded[8];
    CHECK(pool_load(copyDir.c_str(), reloaded, 8) == afterWrite);

    Tmp broken("lift-pool-ver");
    std::filesystem::create_directories(broken.path / "samples");
    {
        std::ofstream out(broken.path / "samples" / "index.json");
        out << "{ \"version\": 2, \"items\": [] }\n";
    }
    CHECK(pool_load(broken.path.string().c_str(), scratch, 8) == -1);
}

struct Fixture {
    int listenFd = -1;
    int port = 0;
    std::atomic<int> pull{0};
    std::atomic<int> partial{0};
    std::thread thr;
    std::vector<unsigned char> body;

    ~Fixture() {
        pull.store(1);
        if (listenFd >= 0) {
            ::shutdown(listenFd, SHUT_RDWR);
            ::close(listenFd);
            listenFd = -1;
        }
        if (thr.joinable()) {
            thr.join();
        }
    }

    bool open(const std::vector<unsigned char>& mp3) {
        const int interval = 512;
        const unsigned char meta[16] = {'m', 'e', 't', 'a', 'd', 'a', 't', 'a', '-', 'b', 'l', 'o', 'c', 'k', '!', '!'};
        size_t cursor = 0;
        while (cursor < mp3.size()) {
            const size_t n = mp3.size() - cursor > static_cast<size_t>(interval) ? static_cast<size_t>(interval) : mp3.size() - cursor;
            body.insert(body.end(), mp3.begin() + static_cast<std::ptrdiff_t>(cursor), mp3.begin() + static_cast<std::ptrdiff_t>(cursor + n));
            cursor += n;
            if (n == static_cast<size_t>(interval)) {
                body.push_back(1);
                body.insert(body.end(), meta, meta + 16);
            }
        }
        listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd < 0) {
            return false;
        }
        const int one = 1;
        setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 || listen(listenFd, 1) < 0) {
            return false;
        }
        sockaddr_in bound{};
        socklen_t len = sizeof bound;
        if (getsockname(listenFd, reinterpret_cast<sockaddr*>(&bound), &len) < 0) {
            return false;
        }
        port = ntohs(bound.sin_port);
        thr = std::thread([this] { serve(); });
        return port > 0;
    }

    void serve() {
        const int fd = listenFd;
        sockaddr_in addr{};
        socklen_t len = sizeof addr;
        const int client = ::accept(fd, reinterpret_cast<sockaddr*>(&addr), &len);
        if (client < 0) {
            return;
        }
        const int one = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        timeval tv{};
        tv.tv_sec = 2;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        std::string req;
        char buf[512];
        while (req.find("\r\n\r\n") == std::string::npos && req.size() < 4096) {
            const ssize_t n = ::recv(client, buf, sizeof buf, 0);
            if (n <= 0) {
                break;
            }
            req.append(buf, buf + n);
        }
        const char* head = "HTTP/1.0 200 OK\r\nicy-metaint: 512\r\nContent-Type: audio/mpeg\r\n\r\n";
        ::send(client, head, std::strlen(head), 0);
        const size_t prefix = body.size() > 4096 ? 4096 : body.size();
        size_t sent = 0;
        while (sent < prefix) {
            const ssize_t n = ::send(client, body.data() + sent, prefix - sent, 0);
            if (n <= 0) {
                break;
            }
            sent += static_cast<size_t>(n);
        }
        partial.store(1);
        for (int i = 0; i < 2000 && pull.load() == 0; ++i) {
            ::usleep(10000);
        }
        ::close(client);
    }
};

int elapsed_ms(std::chrono::steady_clock::time_point start) {
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}

void test_radio() {
    QuietRadio quiet;
    const std::filesystem::path root{LIFT_SOURCE_DIR};
    RadioEntry entries[4];
    CHECK(radio_load_directory((root / "presets/radio.json").string().c_str(), entries, 4) == 0);

    Tmp list("lift-radio-list");
    {
        std::ofstream out(list.path / "radio-user.json");
        out << "[{ \"name\": \"fixture\", \"url\": \"http://127.0.0.1/live\", \"codec\": \"mp3\" }]\n";
    }
    CHECK(radio_load_directory((list.path / "radio-user.json").string().c_str(), entries, 4) == 1);
    CHECK(std::strcmp(entries[0].name, "fixture") == 0);
    CHECK(std::strcmp(entries[0].codec, "mp3") == 0);

    const int opusFrames = decode_fixture("opus", root / "tests/fixtures/tone.opus");
    const int vorbisFrames = decode_fixture("vorbis", root / "tests/fixtures/tone.ogg");
    const int mp3Frames = decode_fixture("mp3", root / "tests/fixtures/tone.mp3");
    if (opusFrames <= 1000 || vorbisFrames <= 1000 || mp3Frames <= 20000) {
        std::cerr << "codec frames opus " << opusFrames << " vorbis " << vorbisFrames << " mp3 " << mp3Frames << "\n";
    }
    CHECK(opusFrames > 1000);
    CHECK(vorbisFrames > 1000);
    CHECK(mp3Frames > 20000);

    float idle[8];
    const auto idleStart = std::chrono::steady_clock::now();
    CHECK(lift_radio_read(idle, 4) == 1);
    CHECK(elapsed_ms(idleStart) < 100);

    RadioRequest bad{};
    bad.url = "https://127.0.0.1/live";
    bad.codec = "mp3";
    CHECK(lift_radio_tune(&bad) == 1);
    bad.url = "http://127.0.0.1/live";
    bad.codec = "aac";
    CHECK(lift_radio_tune(&bad) == 1);

    Tmp project("lift-radio");
    const std::string dir = project.path.string();
    write_wav(project.path / "one.wav", 48000, 2, 32, 16384, -16384, 0);
    char reason[128] = {};
    PoolItem imported{};
    CHECK(import_ok(dir.c_str(), (project.path / "one.wav").string().c_str(), imported, reason));
    PoolItem dup{};
    CHECK(pool_import(dir.c_str(), (project.path / "one.wav").string().c_str(), &dup, reason, 128) == PoolStatus::Duplicate);

    Fixture server;
    const std::vector<unsigned char> mp3 = read_bytes(root / "tests/fixtures/tone.mp3");
    CHECK(server.open(mp3));
    char url[64];
    std::snprintf(url, sizeof url, "http://127.0.0.1:%d/live", server.port);
    RadioRequest request{};
    request.name = "fixture";
    request.url = url;
    request.codec = "mp3";
    request.project = dir.c_str();
    CHECK(lift_radio_tune(&request) == 0);

    std::vector<float> recL;
    std::vector<float> recR;
    // This fixture stays under 0.05 until sample 2269 (MP3 priming).
    constexpr int kListen = 4096;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (static_cast<int>(recL.size()) < kListen && std::chrono::steady_clock::now() < deadline) {
        float frame[2];
        if (lift_radio_read(frame, 1) == 0) {
            recL.push_back(frame[0]);
            recR.push_back(frame[1]);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    if (static_cast<int>(recL.size()) < kListen) {
        std::cerr << "radio frames " << recL.size() << "\n";
    }
    CHECK(static_cast<int>(recL.size()) >= kListen);

    int best = 0;
    float bestPeak = 0.f;
    for (int i = 0; i < static_cast<int>(recL.size()); ++i) {
        const float a = std::fabs(recL[static_cast<size_t>(i)]);
        if (a > bestPeak) {
            bestPeak = a;
            best = i;
        }
    }
    CHECK(bestPeak > 0.05f);
    constexpr int n = 128;
    int at = best > 64 ? best - 64 : 0;
    if (at + n > static_cast<int>(recL.size())) {
        at = static_cast<int>(recL.size()) - n;
    }
    TapeRuntime rt;
    transport_init(rt);
    prepare_tracks(rt, n);
    for (int t = 0; t < kTrackCount; ++t) {
        rt.mute[t] = t != 0;
        rt.fader[t] = 1.f;
    }
    rt.arm = 0;
    transport_set_varispeed(rt, 4.f);
    rt.reverse = true;
    rt.pos = 0.0;
    rt.recording = true;
    rt.playing = true;
    std::vector<float> outL(static_cast<size_t>(n), 0.f);
    std::vector<float> outR(static_cast<size_t>(n), 0.f);
    process_block(rt, recL.data() + at, recR.data() + at, outL.data(), outR.data(), n);
    CHECK(rt.pos == static_cast<double>(n));
    float printed = 0.f;
    for (int i = 0; i < n; ++i) {
        const float a = std::fabs(rt.ch[0][0][i]);
        if (a > printed) {
            printed = a;
        }
    }
    CHECK(printed > 0.01f);
    release_tracks(rt);

    for (int i = 0; i < 200 && server.partial.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(server.partial.load() == 1);
    CHECK(server.pull.load() == 0);
    std::vector<float> big(48000u * 2u);
    const auto stalled = std::chrono::steady_clock::now();
    const int underrun = lift_radio_read(big.data(), 48000);
    const int stalledMs = elapsed_ms(stalled);
    CHECK(underrun == 2);
    CHECK(stalledMs < 100);

    server.pull.store(1);
    const auto after = std::chrono::steady_clock::now();
    const int again = lift_radio_read(big.data(), 48000);
    CHECK(again == 2 || again == 1);
    CHECK(elapsed_ms(after) < 100);

    const int captured = lift_radio_capture(nullptr, 0);
    if (captured != 2) {
        std::cerr << "capture " << captured << "\n";
    }
    CHECK(captured == 2);

    TapeRuntime stored;
    transport_init(stored);
    prepare_tracks(stored, 2048);
    ProjectInfo info;
    project_info_init(info);
    info.frames = 2048;
    info.tempo = 113.f;
    CHECK(project_write(dir.c_str(), info, stored) == LoadStatus::Ok);
    release_tracks(stored);

    Tmp copied("lift-radio-copy");
    std::filesystem::copy(project.path, copied.path / "project", std::filesystem::copy_options::recursive);
    const std::string copyDir = (copied.path / "project").string();
    ProjectInfo loaded;
    project_info_init(loaded);
    CHECK(project_read_info(copyDir.c_str(), loaded) == LoadStatus::Ok);
    CHECK(loaded.tempo == 113.f);
    PoolItem items[8];
    const int count = pool_load(copyDir.c_str(), items, 8);
    CHECK(count == 2);
    bool sawImport = false;
    bool sawCapture = false;
    for (int i = 0; i < count && i < 8; ++i) {
        if (std::strcmp(items[i].hash, imported.hash) == 0) {
            sawImport = items[i].frames == 32;
        } else if (items[i].frames > 2000) {
            const int probeN = items[i].frames < 4096 ? items[i].frames : 4096;
            std::vector<float> probeL(static_cast<size_t>(probeN));
            std::vector<float> probeR(static_cast<size_t>(probeN));
            if (pool_read(copyDir.c_str(), items[i].hash, probeL.data(), probeR.data(), probeN) == PoolStatus::Ok) {
                for (int s = 0; s < probeN; ++s) {
                    if (std::fabs(probeL[static_cast<size_t>(s)]) > 0.01f ||
                        std::fabs(probeR[static_cast<size_t>(s)]) > 0.01f) {
                        sawCapture = true;
                    }
                }
            }
        }
    }
    CHECK(sawImport);
    CHECK(sawCapture);
}

}  // namespace

int run_pool_suite(const char* only) {
    const bool all = only == nullptr || only[0] == '\0';
    const bool pool = all || std::strcmp(only, "pool") == 0;
    const bool radio = all || std::strcmp(only, "radio") == 0;
    if (!pool && !radio) {
        return 0;
    }
    const int before = g_fails;
    if (pool) {
        test_units();
        test_pool();
    }
    if (radio) {
        test_radio();
    }
    return g_fails - before;
}
