// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "project/project.h"

#include "audio/prepare.h"
#include "tape/character.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

void copy_mode(char* dst, int n, const std::string& mode) {
    const char* src = (mode == "bay") ? "bay" : "compact";
    std::snprintf(dst, static_cast<size_t>(n), "%s", src);
}

bool read_file(const fs::path& path, std::string& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool field_pos(const std::string& text, const char* key, size_t& pos) {
    const std::string pat = std::string("\"") + key + "\"";
    const size_t keyAt = text.find(pat);
    if (keyAt == std::string::npos) {
        return false;
    }
    const size_t colon = text.find(':', keyAt + pat.size());
    if (colon == std::string::npos) {
        return false;
    }
    pos = colon + 1;
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\n' || text[pos] == '\t')) {
        ++pos;
    }
    return pos < text.size();
}

bool parse_int(const std::string& text, const char* key, int& value) {
    size_t pos = 0;
    if (!field_pos(text, key, pos)) {
        return false;
    }
    try {
        size_t used = 0;
        value = std::stoi(text.substr(pos), &used);
        return used > 0;
    } catch (...) {
        return false;
    }
}

bool parse_float(const std::string& text, const char* key, float& value) {
    size_t pos = 0;
    if (!field_pos(text, key, pos)) {
        return false;
    }
    try {
        size_t used = 0;
        value = std::stof(text.substr(pos), &used);
        return used > 0;
    } catch (...) {
        return false;
    }
}

bool parse_string(const std::string& text, const char* key, std::string& value) {
    size_t pos = 0;
    if (!field_pos(text, key, pos)) {
        return false;
    }
    if (text[pos] != '"') {
        return false;
    }
    const size_t end = text.find('"', pos + 1);
    if (end == std::string::npos) {
        return false;
    }
    value = text.substr(pos + 1, end - pos - 1);
    return true;
}

void write_u32(std::ostream& out, uint32_t v) {
    const unsigned char b[4] = {
        static_cast<unsigned char>(v & 255u),
        static_cast<unsigned char>((v >> 8) & 255u),
        static_cast<unsigned char>((v >> 16) & 255u),
        static_cast<unsigned char>((v >> 24) & 255u),
    };
    out.write(reinterpret_cast<const char*>(b), 4);
}

void write_u16(std::ostream& out, uint16_t v) {
    const unsigned char b[2] = {
        static_cast<unsigned char>(v & 255u),
        static_cast<unsigned char>((v >> 8) & 255u),
    };
    out.write(reinterpret_cast<const char*>(b), 2);
}

bool read_u32(std::istream& in, uint32_t& v) {
    unsigned char b[4];
    in.read(reinterpret_cast<char*>(b), 4);
    if (!in) {
        return false;
    }
    v = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
        (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
    return true;
}

bool read_u16(std::istream& in, uint16_t& v) {
    unsigned char b[2];
    in.read(reinterpret_cast<char*>(b), 2);
    if (!in) {
        return false;
    }
    v = static_cast<uint16_t>(b[0] | (b[1] << 8));
    return true;
}

bool write_track_wav(const fs::path& path, const float* left, const float* right, int frames) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    const uint32_t dataBytes = static_cast<uint32_t>(frames) * 2u * 4u;
    const uint32_t riffBytes = 36u + dataBytes;
    out.write("RIFF", 4);
    write_u32(out, riffBytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    write_u32(out, 16);
    write_u16(out, 3);
    write_u16(out, 2);
    write_u32(out, static_cast<uint32_t>(kSampleRate));
    write_u32(out, static_cast<uint32_t>(kSampleRate) * 8u);
    write_u16(out, 8);
    write_u16(out, 32);
    out.write("data", 4);
    write_u32(out, dataBytes);
    for (int i = 0; i < frames; ++i) {
        out.write(reinterpret_cast<const char*>(left + i), 4);
        out.write(reinterpret_cast<const char*>(right + i), 4);
    }
    return static_cast<bool>(out);
}

bool read_track_wav(const fs::path& path, float* left, float* right, int frames) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    char tag[4];
    in.read(tag, 4);
    if (std::strncmp(tag, "RIFF", 4) != 0) {
        return false;
    }
    uint32_t riffBytes = 0;
    if (!read_u32(in, riffBytes)) {
        return false;
    }
    in.read(tag, 4);
    if (std::strncmp(tag, "WAVE", 4) != 0) {
        return false;
    }
    bool gotFmt = false;
    bool gotData = false;
    uint16_t format = 0;
    uint16_t channels = 0;
    uint32_t rate = 0;
    uint16_t bits = 0;
    while (in && (!gotFmt || !gotData)) {
        in.read(tag, 4);
        uint32_t size = 0;
        if (!in || !read_u32(in, size)) {
            return false;
        }
        if (std::strncmp(tag, "fmt ", 4) == 0) {
            if (size < 16) {
                return false;
            }
            if (!read_u16(in, format) || !read_u16(in, channels) || !read_u32(in, rate)) {
                return false;
            }
            uint32_t byteRate = 0;
            uint16_t align = 0;
            if (!read_u32(in, byteRate) || !read_u16(in, align) || !read_u16(in, bits)) {
                return false;
            }
            if (size > 16) {
                in.seekg(static_cast<std::streamoff>(size - 16), std::ios::cur);
            }
            gotFmt = true;
        } else if (std::strncmp(tag, "data", 4) == 0) {
            if (!gotFmt || format != 3 || channels != 2 || bits != 32 || rate != static_cast<uint32_t>(kSampleRate)) {
                return false;
            }
            const uint32_t need = static_cast<uint32_t>(frames) * 8u;
            if (size < need) {
                return false;
            }
            for (int i = 0; i < frames; ++i) {
                in.read(reinterpret_cast<char*>(left + i), 4);
                in.read(reinterpret_cast<char*>(right + i), 4);
                if (!in) {
                    return false;
                }
            }
            gotData = true;
        } else {
            in.seekg(static_cast<std::streamoff>(size), std::ios::cur);
        }
    }
    return gotFmt && gotData;
}

fs::path tape_dir(const fs::path& root, int index) {
    return root / "tapes" / ("tape-" + std::to_string(index));
}

}  // namespace

void project_info_init(ProjectInfo& info) {
    info.version = kProjectVersion;
    info.tempo = 120.f;
    info.tapeIndex = 0;
    info.frames = kDefaultFrames;
    info.sampleRate = kSampleRate;
    info.character = 0;
    copy_mode(info.uiMode, static_cast<int>(sizeof info.uiMode), "compact");
}

LoadStatus project_read_info(const char* dir, ProjectInfo& info) {
    if (dir == nullptr) {
        return LoadStatus::Io;
    }
    std::string text;
    if (!read_file(fs::path(dir) / "project.json", text)) {
        return LoadStatus::Io;
    }
    int version = 0;
    if (!parse_int(text, "version", version)) {
        return LoadStatus::BadVersion;
    }
    if (version > kProjectVersion) {
        return LoadStatus::NewerVersion;
    }
    if (version != kProjectVersion) {
        return LoadStatus::BadVersion;
    }
    ProjectInfo next = info;
    next.version = version;
    if (!parse_float(text, "tempo", next.tempo)) {
        return LoadStatus::BadVersion;
    }
    if (!parse_int(text, "tapeIndex", next.tapeIndex) || next.tapeIndex < 0 || next.tapeIndex > 7) {
        return LoadStatus::BadVersion;
    }
    if (!parse_int(text, "frames", next.frames) || next.frames <= 0) {
        return LoadStatus::BadVersion;
    }
    if (!parse_int(text, "sampleRate", next.sampleRate) || next.sampleRate != kSampleRate) {
        return LoadStatus::BadVersion;
    }
    std::string mode;
    if (!parse_string(text, "uiMode", mode)) {
        return LoadStatus::BadVersion;
    }
    copy_mode(next.uiMode, static_cast<int>(sizeof next.uiMode), mode);
    std::string character;
    if (!parse_string(text, "character", character)) {
        return LoadStatus::BadVersion;
    }
    const int index = character_index(character.c_str());
    if (index < 0) {
        return LoadStatus::BadVersion;
    }
    next.character = index;
    info = next;
    return LoadStatus::Ok;
}

LoadStatus project_write(const char* dir, const ProjectInfo& info, const TapeRuntime& rt) {
    if (dir == nullptr || info.version != kProjectVersion) {
        return LoadStatus::BadVersion;
    }
    if (rt.frames != info.frames) {
        return LoadStatus::Io;
    }
    const fs::path root(dir);
    const fs::path tape = tape_dir(root, info.tapeIndex);
    std::error_code ec;
    fs::create_directories(tape, ec);
    if (ec) {
        return LoadStatus::Io;
    }
    std::ofstream out(root / "project.json");
    if (!out) {
        return LoadStatus::Io;
    }
    out << "{\n"
        << "  \"version\": " << info.version << ",\n"
        << "  \"tempo\": " << info.tempo << ",\n"
        << "  \"tapeIndex\": " << info.tapeIndex << ",\n"
        << "  \"frames\": " << info.frames << ",\n"
        << "  \"sampleRate\": " << info.sampleRate << ",\n"
        << "  \"uiMode\": \"" << info.uiMode << "\",\n"
        << "  \"character\": \"" << character_row(info.character).name << "\"\n"
        << "}\n";
    if (!out) {
        return LoadStatus::Io;
    }
    for (int t = 0; t < kTrackCount; ++t) {
        if (rt.ch[t][0] == nullptr || rt.ch[t][1] == nullptr) {
            return LoadStatus::Io;
        }
        const fs::path wav = tape / ("tr" + std::to_string(t) + ".wav");
        if (!write_track_wav(wav, rt.ch[t][0], rt.ch[t][1], info.frames)) {
            return LoadStatus::Io;
        }
    }
    return LoadStatus::Ok;
}

LoadStatus project_read_audio(const char* dir, const ProjectInfo& info, TapeRuntime& rt) {
    if (dir == nullptr || info.version != kProjectVersion) {
        return LoadStatus::BadVersion;
    }
    prepare_tracks(rt, info.frames);
    transport_bind_character(rt, info.character);
    const fs::path tape = tape_dir(fs::path(dir), info.tapeIndex);
    for (int t = 0; t < kTrackCount; ++t) {
        const fs::path wav = tape / ("tr" + std::to_string(t) + ".wav");
        if (!read_track_wav(wav, rt.ch[t][0], rt.ch[t][1], info.frames)) {
            return LoadStatus::Io;
        }
    }
    return LoadStatus::Ok;
}
