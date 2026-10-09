// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "pool/pool.h"

#include "codec/codec.h"
#include "tape/character.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace {

std::mutex g_pool;

void set_reason(char* reason, int n, const char* text) {
    if (reason == nullptr || n <= 0) {
        return;
    }
    std::snprintf(reason, static_cast<size_t>(n), "%s", text);
}

void copy_field(char* dst, int n, const std::string& text) {
    if (n <= 0) {
        return;
    }
    std::snprintf(dst, static_cast<size_t>(n), "%s", text.c_str());
}

uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32u - n));
}

void sha_block(uint32_t s[8], const unsigned char block[64]) {
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
        0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
        0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
        0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
        0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
        0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
        0xc67178f2u};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) | (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 8) | block[i * 4 + 3];
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t t1 = h + S1 + ch + k[i] + w[i];
        const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s[0] += a;
    s[1] += b;
    s[2] += c;
    s[3] += d;
    s[4] += e;
    s[5] += f;
    s[6] += g;
    s[7] += h;
}

bool find_string(const std::string& text, size_t& cursor, const char* key, std::string& value) {
    const std::string pattern = std::string("\"") + key + "\"";
    const size_t keyAt = text.find(pattern, cursor);
    if (keyAt == std::string::npos) {
        return false;
    }
    const size_t colon = text.find(':', keyAt + pattern.size());
    if (colon == std::string::npos) {
        return false;
    }
    const size_t open = text.find('"', colon + 1);
    if (open == std::string::npos) {
        return false;
    }
    const size_t close = text.find('"', open + 1);
    if (close == std::string::npos) {
        return false;
    }
    value = text.substr(open + 1, close - open - 1);
    cursor = close + 1;
    return true;
}

bool find_int(const std::string& text, size_t cursor, const char* key, long long& value) {
    const std::string pattern = std::string("\"") + key + "\"";
    const size_t keyAt = text.find(pattern, cursor);
    if (keyAt == std::string::npos) {
        return false;
    }
    const size_t colon = text.find(':', keyAt + pattern.size());
    if (colon == std::string::npos) {
        return false;
    }
    size_t i = colon + 1;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\n')) {
        ++i;
    }
    try {
        size_t used = 0;
        value = std::stoll(text.substr(i), &used);
        return used > 0;
    } catch (...) {
        return false;
    }
}

std::filesystem::path sample_dir(const std::filesystem::path& root) {
    return root / "samples";
}

std::filesystem::path index_path(const std::filesystem::path& root) {
    return sample_dir(root) / "index.json";
}

bool load_items(const std::filesystem::path& root, std::vector<PoolItem>& items, int& version) {
    items.clear();
    version = 0;
    std::ifstream in(index_path(root));
    if (!in) {
        version = 1;
        return true;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    long long ver = 0;
    if (!find_int(text, 0, "version", ver)) {
        return false;
    }
    version = static_cast<int>(ver);
    if (version > 1) {
        return false;
    }
    size_t cursor = 0;
    while (cursor < text.size()) {
        const size_t hashAt = text.find("\"hash\"", cursor);
        if (hashAt == std::string::npos) {
            break;
        }
        cursor = hashAt;
        PoolItem item{};
        std::string hash;
        std::string name;
        std::string map;
        if (!find_string(text, cursor, "hash", hash)) {
            return false;
        }
        find_string(text, cursor, "name", name);
        find_string(text, cursor, "map", map);
        long long rootNote = 60;
        long long slices = kPoolSlices;
        long long frames = 0;
        long long rate = kSampleRate;
        long long bytes = 0;
        find_int(text, hashAt, "root", rootNote);
        find_int(text, hashAt, "slices", slices);
        find_int(text, hashAt, "frames", frames);
        find_int(text, hashAt, "rate", rate);
        find_int(text, hashAt, "bytes", bytes);
        copy_field(item.hash, 65, hash);
        copy_field(item.name, 128, name);
        copy_field(item.map, 16, map.empty() ? "slices" : map);
        item.root = static_cast<int>(rootNote);
        item.slices = static_cast<int>(slices);
        item.frames = static_cast<int>(frames);
        item.rate = static_cast<int>(rate);
        item.bytes = static_cast<uint64_t>(bytes);
        items.push_back(item);
    }
    return true;
}

bool store_items(const std::filesystem::path& root, const std::vector<PoolItem>& items) {
    std::error_code ec;
    std::filesystem::create_directories(sample_dir(root), ec);
    if (ec) {
        return false;
    }
    const auto path = index_path(root);
    const auto tmp = path.string() + ".tmp";
    std::ofstream out(tmp);
    if (!out) {
        return false;
    }
    out << "{\n  \"version\": 1,\n  \"items\": [\n";
    for (size_t i = 0; i < items.size(); ++i) {
        const PoolItem& item = items[i];
        out << "    {\"hash\":\"" << item.hash << "\",\"name\":\"" << item.name << "\",\"map\":\"" << item.map
            << "\",\"root\":" << item.root << ",\"slices\":" << item.slices << ",\"frames\":" << item.frames
            << ",\"rate\":" << item.rate << ",\"bytes\":" << item.bytes << "}";
        if (i + 1 < items.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n}\n";
    out.close();
    if (!out) {
        return false;
    }
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

int out_frames_exact(int frames, int rate) {
    if (frames <= 0 || rate <= 0) {
        return 0;
    }
    const double scaled = static_cast<double>(frames) * static_cast<double>(kSampleRate) / static_cast<double>(rate);
    return static_cast<int>(scaled + 0.5);
}

std::string clean_name(const std::string& raw) {
    std::string name = std::filesystem::path(raw).filename().string();
    for (char& c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '-' || c == '_' || c == ' ';
        if (!ok) {
            c = '_';
        }
    }
    if (name.empty()) {
        name = "pool";
    }
    if (name.size() > 120) {
        name.resize(120);
    }
    return name;
}

void resample(const Decoded& in, std::vector<float>& left, std::vector<float>& right) {
    if (in.rate == kSampleRate) {
        left = in.left;
        right = in.right;
        return;
    }
    const int srcN = static_cast<int>(in.left.size());
    const int dstN = out_frames_exact(srcN, in.rate);
    left.assign(static_cast<size_t>(dstN), 0.f);
    right.assign(static_cast<size_t>(dstN), 0.f);
    if (srcN <= 0 || dstN <= 0) {
        return;
    }
    for (int i = 0; i < dstN; ++i) {
        const double src = static_cast<double>(i) * static_cast<double>(in.rate) / static_cast<double>(kSampleRate);
        int i0 = static_cast<int>(src);
        if (i0 >= srcN - 1) {
            i0 = srcN - 1;
            left[static_cast<size_t>(i)] = in.left[static_cast<size_t>(i0)];
            right[static_cast<size_t>(i)] = in.right[static_cast<size_t>(i0)];
            continue;
        }
        const float t = static_cast<float>(src - static_cast<double>(i0));
        const float l0 = in.left[static_cast<size_t>(i0)];
        const float l1 = in.left[static_cast<size_t>(i0 + 1)];
        const float r0 = in.right[static_cast<size_t>(i0)];
        const float r1 = in.right[static_cast<size_t>(i0 + 1)];
        left[static_cast<size_t>(i)] = l0 + (l1 - l0) * t;
        right[static_cast<size_t>(i)] = r0 + (r1 - r0) * t;
    }
}

PoolStatus commit_pcm(
    const std::filesystem::path& root,
    const std::string& hash,
    const std::string& name,
    const float* left,
    const float* right,
    int frames,
    PoolItem* out,
    char* reason,
    int reasonLen) {
    std::vector<PoolItem> items;
    int version = 1;
    if (!load_items(root, items, version) || version > 1) {
        set_reason(reason, reasonLen, "could not read the file");
        return PoolStatus::Io;
    }
    for (const PoolItem& item : items) {
        if (hash == item.hash) {
            if (out != nullptr) {
                *out = item;
            }
            set_reason(reason, reasonLen, "already in the pool");
            return PoolStatus::Duplicate;
        }
    }
    const uint64_t add = 44ull + static_cast<uint64_t>(frames) * 8ull;
    uint64_t existing = 0;
    for (const PoolItem& item : items) {
        existing += item.bytes;
    }
    const PoolStatus room = pool_admit(existing, add);
    if (room == PoolStatus::OverCap) {
        set_reason(reason, reasonLen, "past 2 GB");
        return room;
    }
    std::error_code ec;
    std::filesystem::create_directories(sample_dir(root), ec);
    const auto wav = sample_dir(root) / (hash + ".wav");
    if (!write_stereo_wav(wav.string().c_str(), left, right, frames)) {
        set_reason(reason, reasonLen, "could not read the file");
        return PoolStatus::Io;
    }
    PoolItem item{};
    copy_field(item.hash, 65, hash);
    copy_field(item.name, 128, clean_name(name));
    copy_field(item.map, 16, "slices");
    item.root = 60;
    item.slices = kPoolSlices;
    item.frames = frames;
    item.rate = kSampleRate;
    item.bytes = static_cast<uint64_t>(std::filesystem::file_size(wav, ec));
    if (ec) {
        item.bytes = add;
    }
    items.push_back(item);
    if (!store_items(root, items)) {
        return PoolStatus::Io;
    }
    if (out != nullptr) {
        *out = item;
    }
    if (room == PoolStatus::Warn) {
        set_reason(reason, reasonLen, "pool is at 1.5 GB");
    }
    return room;
}

}  // namespace

PoolStatus pool_admit(uint64_t existing, uint64_t add) {
    if (existing >= kPoolCapBytes || add > kPoolCapBytes - existing) {
        return PoolStatus::OverCap;
    }
    if (existing + add >= kPoolWarnBytes) {
        return PoolStatus::Warn;
    }
    return PoolStatus::Ok;
}

void pool_slice(int frames, int slices, int key, int& start, int& length) {
    if (slices < 1) {
        slices = 1;
    }
    if (key < 0) {
        key = 0;
    }
    if (key >= slices) {
        key = slices - 1;
    }
    if (frames < 0) {
        frames = 0;
    }
    start = static_cast<int>((static_cast<long long>(key) * frames) / slices);
    const int end = static_cast<int>((static_cast<long long>(key + 1) * frames) / slices);
    length = end - start;
}

float pool_pitch_ratio(int root, int note) {
    return std::pow(2.f, static_cast<float>(note - root) / 12.f);
}

void pool_hash_bytes(const void* data, int n, char hex[65]) {
    uint32_t s[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    const auto* bytes = static_cast<const unsigned char*>(data);
    uint64_t bits = 0;
    unsigned char block[64];
    int filled = 0;
    const int count = n < 0 ? 0 : n;
    for (int i = 0; i < count; ++i) {
        block[filled++] = bytes[i];
        if (filled == 64) {
            sha_block(s, block);
            bits += 512;
            filled = 0;
        }
    }
    bits += static_cast<uint64_t>(filled) * 8u;
    block[filled++] = 0x80;
    if (filled > 56) {
        while (filled < 64) {
            block[filled++] = 0;
        }
        sha_block(s, block);
        filled = 0;
    }
    while (filled < 56) {
        block[filled++] = 0;
    }
    for (int i = 7; i >= 0; --i) {
        block[filled++] = static_cast<unsigned char>((bits >> (i * 8)) & 255u);
    }
    sha_block(s, block);
    static const char* digits = "0123456789abcdef";
    for (int i = 0; i < 8; ++i) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            hex[i * 8 + (28 - shift) / 4] = digits[(s[i] >> shift) & 15u];
        }
    }
    hex[64] = '\0';
}

PoolStatus pool_import(const char* project, const char* path, PoolItem* out, char* reason, int reasonLen) {
    if (project == nullptr || path == nullptr) {
        set_reason(reason, reasonLen, "could not read the file");
        return PoolStatus::Io;
    }
    std::error_code sizeError;
    const auto fileBytes = std::filesystem::file_size(path, sizeError);
    if (sizeError) {
        set_reason(reason, reasonLen, "could not read the file");
        return PoolStatus::Io;
    }
    if (fileBytes >= kPoolCapBytes) {
        set_reason(reason, reasonLen, "past 2 GB");
        return PoolStatus::OverCap;
    }
    std::lock_guard<std::mutex> lock(g_pool);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        set_reason(reason, reasonLen, "could not read the file");
        return PoolStatus::Io;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() != fileBytes) {
        set_reason(reason, reasonLen, "could not read the file");
        return PoolStatus::Io;
    }
    char hash[65];
    pool_hash_bytes(bytes.data(), static_cast<int>(bytes.size()), hash);
    std::vector<PoolItem> items;
    int version = 1;
    if (!load_items(project, items, version) || version > 1) {
        return PoolStatus::Io;
    }
    for (const PoolItem& item : items) {
        if (std::strcmp(item.hash, hash) == 0) {
            if (out != nullptr) {
                *out = item;
            }
            set_reason(reason, reasonLen, "already in the pool");
            return PoolStatus::Duplicate;
        }
    }
    Decoded decoded;
    const DecodeStatus status = decode_file(path, decoded, reason, reasonLen);
    if (status == DecodeStatus::Duration) {
        return PoolStatus::Duration;
    }
    if (status == DecodeStatus::Io) {
        return PoolStatus::Io;
    }
    if (status != DecodeStatus::Ok) {
        set_reason(reason, reasonLen, "need wav, aif, flac, or mp3");
        return PoolStatus::Format;
    }
    std::vector<float> left;
    std::vector<float> right;
    resample(decoded, left, right);
    if (static_cast<int>(left.size()) > kPoolMaxFrames) {
        set_reason(reason, reasonLen, "longer than 30 minutes");
        return PoolStatus::Duration;
    }
    return commit_pcm(project, hash, clean_name(path), left.data(), right.data(), static_cast<int>(left.size()), out, reason, reasonLen);
}

PoolStatus pool_add_pcm(
    const char* project,
    const char* name,
    const float* left,
    const float* right,
    int frames,
    PoolItem* out,
    char* reason,
    int reasonLen) {
    if (project == nullptr || left == nullptr || right == nullptr || frames < 0) {
        return PoolStatus::Io;
    }
    if (frames > kCaptureFrames) {
        frames = kCaptureFrames;
    }
    if (frames > kPoolMaxFrames) {
        set_reason(reason, reasonLen, "longer than 30 minutes");
        return PoolStatus::Duration;
    }
    std::lock_guard<std::mutex> lock(g_pool);
    std::vector<float> packed(static_cast<size_t>(frames) * 2u);
    for (int i = 0; i < frames; ++i) {
        packed[static_cast<size_t>(i * 2)] = left[i];
        packed[static_cast<size_t>(i * 2 + 1)] = right[i];
    }
    char hash[65];
    pool_hash_bytes(packed.data(), static_cast<int>(packed.size() * sizeof(float)), hash);
    return commit_pcm(project, hash, name != nullptr ? name : "capture", left, right, frames, out, reason, reasonLen);
}

int pool_load(const char* project, PoolItem* itemsOut, int cap) {
    if (project == nullptr || itemsOut == nullptr || cap < 0) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_pool);
    std::vector<PoolItem> items;
    int version = 1;
    if (!load_items(project, items, version) || version > 1) {
        return -1;
    }
    const int n = static_cast<int>(items.size());
    const int copy = n < cap ? n : cap;
    for (int i = 0; i < copy; ++i) {
        itemsOut[i] = items[static_cast<size_t>(i)];
    }
    return n;
}

PoolStatus pool_set_map(const char* project, const char* hash, const char* map, int root) {
    if (project == nullptr || hash == nullptr || map == nullptr) {
        return PoolStatus::Io;
    }
    if (std::strcmp(map, "slices") != 0 && std::strcmp(map, "chromatic") != 0) {
        return PoolStatus::Format;
    }
    std::lock_guard<std::mutex> lock(g_pool);
    std::vector<PoolItem> items;
    int version = 1;
    if (!load_items(project, items, version) || version > 1) {
        return PoolStatus::Io;
    }
    for (PoolItem& item : items) {
        if (std::strcmp(item.hash, hash) == 0) {
            copy_field(item.map, 16, map);
            item.root = root;
            if (!store_items(project, items)) {
                return PoolStatus::Io;
            }
            return PoolStatus::Ok;
        }
    }
    return PoolStatus::Io;
}

PoolStatus pool_read(const char* project, const char* hash, float* left, float* right, int frames) {
    if (project == nullptr || hash == nullptr || left == nullptr || right == nullptr) {
        return PoolStatus::Io;
    }
    const auto wav = sample_dir(project) / (std::string(hash) + ".wav");
    if (!read_stereo_wav(wav.string().c_str(), left, right, frames)) {
        return PoolStatus::Io;
    }
    return PoolStatus::Ok;
}

uint64_t pool_bytes(const char* project) {
    if (project == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_pool);
    std::vector<PoolItem> items;
    int version = 1;
    if (!load_items(project, items, version)) {
        return 0;
    }
    uint64_t n = 0;
    for (const PoolItem& item : items) {
        n += item.bytes;
    }
    return n;
}
