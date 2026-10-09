// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "codec/codec.h"

#include "pool/pool.h"
#include "tape/character.h"

#include "dr_flac.h"
#include "minimp3.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace {

void set_reason(char* reason, int n, const char* text) {
    if (reason == nullptr || n <= 0) {
        return;
    }
    std::snprintf(reason, static_cast<size_t>(n), "%s", text);
}

int out_frames(int frames, int rate) {
    if (frames <= 0 || rate <= 0) {
        return 0;
    }
    const double scaled = static_cast<double>(frames) * static_cast<double>(kSampleRate) / static_cast<double>(rate);
    if (scaled > static_cast<double>(kPoolMaxFrames)) {
        return kPoolMaxFrames + 1;
    }
    return static_cast<int>(scaled + 0.5);
}

bool read_all(const char* path, std::vector<unsigned char>& bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff n = in.tellg();
    if (n < 0 || static_cast<uint64_t>(n) >= kPoolCapBytes) {
        return false;
    }
    in.seekg(0);
    bytes.resize(static_cast<size_t>(n));
    if (n == 0) {
        return true;
    }
    in.read(reinterpret_cast<char*>(bytes.data()), n);
    return static_cast<bool>(in);
}

uint16_t ru16(const unsigned char* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t ru32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

uint16_t bu16(const unsigned char* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t bu32(const unsigned char* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

int extended_rate(const unsigned char* b) {
    const int exp = ((b[0] & 0x7f) << 8) | b[1];
    uint64_t mant = 0;
    for (int i = 0; i < 8; ++i) {
        mant = (mant << 8) | b[2 + i];
    }
    if (exp == 0 || mant == 0) {
        return 0;
    }
    const int power = exp - 16383 - 63;
    const double v = static_cast<double>(mant) * std::ldexp(1.0, power);
    if (v < 1.0 || v > 384000.0) {
        return 0;
    }
    return static_cast<int>(v + 0.5);
}

void take_pcm(
    Decoded& out,
    const float* interleaved,
    int frames,
    int channels) {
    out.left.resize(static_cast<size_t>(frames));
    out.right.resize(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        const float l = interleaved[static_cast<size_t>(i * channels)];
        const float r = channels > 1 ? interleaved[static_cast<size_t>(i * channels + 1)] : l;
        out.left[static_cast<size_t>(i)] = l;
        out.right[static_cast<size_t>(i)] = r;
    }
}

DecodeStatus decode_wav(const std::vector<unsigned char>& b, Decoded& out, char* reason, int reasonLen) {
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0) {
        return DecodeStatus::Format;
    }
    bool gotFmt = false;
    uint16_t format = 0;
    uint16_t channels = 0;
    uint32_t rate = 0;
    uint16_t bits = 0;
    size_t pos = 12;
    while (pos + 8 <= b.size()) {
        const unsigned char* p = b.data() + pos;
        const uint32_t size = ru32(p + 4);
        pos += 8;
        if (pos + size > b.size() && std::memcmp(p, "data", 4) != 0) {
            return DecodeStatus::Io;
        }
        if (std::memcmp(p, "fmt ", 4) == 0) {
            if (size < 16 || pos + 16 > b.size()) {
                return DecodeStatus::Format;
            }
            format = ru16(b.data() + pos);
            channels = ru16(b.data() + pos + 2);
            rate = ru32(b.data() + pos + 4);
            bits = ru16(b.data() + pos + 14);
            gotFmt = true;
        } else if (std::memcmp(p, "data", 4) == 0) {
            if (!gotFmt || (format != 1 && format != 3) || (channels != 1 && channels != 2) || rate == 0) {
                set_reason(reason, reasonLen, "need wav, aif, flac, or mp3");
                return DecodeStatus::Format;
            }
            const int bytesPer = channels * (bits / 8);
            if (bytesPer <= 0 || (format == 1 && bits != 16) || (format == 3 && bits != 32)) {
                set_reason(reason, reasonLen, "need wav, aif, flac, or mp3");
                return DecodeStatus::Format;
            }
            const int frames = static_cast<int>(size / static_cast<uint32_t>(bytesPer));
            if (out_frames(frames, static_cast<int>(rate)) > kPoolMaxFrames) {
                set_reason(reason, reasonLen, "longer than 30 minutes");
                return DecodeStatus::Duration;
            }
            if (pos + static_cast<size_t>(frames) * static_cast<size_t>(bytesPer) > b.size()) {
                return DecodeStatus::Io;
            }
            out.rate = static_cast<int>(rate);
            out.left.resize(static_cast<size_t>(frames));
            out.right.resize(static_cast<size_t>(frames));
            const unsigned char* s = b.data() + pos;
            for (int i = 0; i < frames; ++i) {
                float l = 0.f;
                float r = 0.f;
                if (format == 3) {
                    std::memcpy(&l, s, 4);
                    s += 4;
                    if (channels == 2) {
                        std::memcpy(&r, s, 4);
                        s += 4;
                    } else {
                        r = l;
                    }
                } else {
                    const int16_t sl = static_cast<int16_t>(ru16(s));
                    s += 2;
                    l = static_cast<float>(sl) / 32768.f;
                    if (channels == 2) {
                        const int16_t sr = static_cast<int16_t>(ru16(s));
                        s += 2;
                        r = static_cast<float>(sr) / 32768.f;
                    } else {
                        r = l;
                    }
                }
                out.left[static_cast<size_t>(i)] = l;
                out.right[static_cast<size_t>(i)] = r;
            }
            return DecodeStatus::Ok;
        }
        pos += size + (size & 1u);
    }
    set_reason(reason, reasonLen, "need wav, aif, flac, or mp3");
    return DecodeStatus::Format;
}

DecodeStatus decode_aiff(const std::vector<unsigned char>& b, Decoded& out, char* reason, int reasonLen) {
    if (b.size() < 12 || std::memcmp(b.data(), "FORM", 4) != 0) {
        return DecodeStatus::Format;
    }
    const bool aifc = std::memcmp(b.data() + 8, "AIFC", 4) == 0;
    if (!aifc && std::memcmp(b.data() + 8, "AIFF", 4) != 0) {
        return DecodeStatus::Format;
    }
    bool gotComm = false;
    uint16_t channels = 0;
    uint32_t frames = 0;
    uint16_t bits = 0;
    int rate = 0;
    size_t pos = 12;
    while (pos + 8 <= b.size()) {
        const unsigned char* p = b.data() + pos;
        const uint32_t size = bu32(p + 4);
        pos += 8;
        if (std::memcmp(p, "COMM", 4) == 0) {
            if (size < 18 || pos + 18 > b.size()) {
                return DecodeStatus::Format;
            }
            channels = bu16(b.data() + pos);
            frames = bu32(b.data() + pos + 2);
            bits = bu16(b.data() + pos + 6);
            rate = extended_rate(b.data() + pos + 8);
            gotComm = true;
        } else if (std::memcmp(p, "SSND", 4) == 0) {
            if (!gotComm || bits != 16 || (channels != 1 && channels != 2) || rate <= 0) {
                set_reason(reason, reasonLen, "need wav, aif, flac, or mp3");
                return DecodeStatus::Format;
            }
            if (out_frames(static_cast<int>(frames), rate) > kPoolMaxFrames) {
                set_reason(reason, reasonLen, "longer than 30 minutes");
                return DecodeStatus::Duration;
            }
            if (pos + 8 > b.size()) {
                return DecodeStatus::Io;
            }
            const uint32_t offset = bu32(b.data() + pos);
            const size_t data = pos + 8 + offset;
            const size_t nbytes = static_cast<size_t>(frames) * channels * 2u;
            if (data + nbytes > b.size()) {
                return DecodeStatus::Io;
            }
            out.rate = rate;
            out.left.resize(frames);
            out.right.resize(frames);
            const unsigned char* s = b.data() + data;
            for (uint32_t i = 0; i < frames; ++i) {
                const int16_t sl = static_cast<int16_t>(bu16(s));
                s += 2;
                const float l = static_cast<float>(sl) / 32768.f;
                float r = l;
                if (channels == 2) {
                    r = static_cast<float>(static_cast<int16_t>(bu16(s))) / 32768.f;
                    s += 2;
                }
                out.left[i] = l;
                out.right[i] = r;
            }
            return DecodeStatus::Ok;
        }
        pos += size + (size & 1u);
    }
    set_reason(reason, reasonLen, "need wav, aif, flac, or mp3");
    return DecodeStatus::Format;
}

DecodeStatus decode_flac(const std::vector<unsigned char>& b, Decoded& out, char* reason, int reasonLen) {
    drflac* flac = drflac_open_memory(b.data(), b.size(), nullptr);
    if (flac == nullptr) {
        set_reason(reason, reasonLen, "need wav, aif, flac, or mp3");
        return DecodeStatus::Format;
    }
    const int channels = flac->channels;
    const int rate = static_cast<int>(flac->sampleRate);
    const auto total = flac->totalPCMFrameCount;
    if (channels < 1 || channels > 2 || rate <= 0 || total == 0 || total > 0x7fffffff) {
        drflac_close(flac);
        set_reason(reason, reasonLen, "need wav, aif, flac, or mp3");
        return DecodeStatus::Format;
    }
    if (out_frames(static_cast<int>(total), rate) > kPoolMaxFrames) {
        drflac_close(flac);
        set_reason(reason, reasonLen, "longer than 30 minutes");
        return DecodeStatus::Duration;
    }
    std::vector<float> interleaved(static_cast<size_t>(total) * static_cast<size_t>(channels));
    const drflac_uint64 got = drflac_read_pcm_frames_f32(flac, total, interleaved.data());
    drflac_close(flac);
    if (got == 0) {
        return DecodeStatus::Io;
    }
    out.rate = rate;
    take_pcm(out, interleaved.data(), static_cast<int>(got), channels);
    return DecodeStatus::Ok;
}

DecodeStatus decode_mp3(const std::vector<unsigned char>& b, Decoded& out, char* reason, int reasonLen) {
    mp3dec_t dec;
    mp3dec_init(&dec);
    int rate = 0;
    int channels = 0;
    size_t offset = 0;
    std::vector<float> interleaved;
    int frames = 0;
    while (offset < b.size()) {
        mp3dec_frame_info_t info;
        int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        const int samples = mp3dec_decode_frame(
            &dec, b.data() + offset, static_cast<int>(b.size() - offset), pcm, &info);
        if (info.frame_bytes == 0) {
            break;
        }
        offset += static_cast<size_t>(info.frame_bytes);
        if (samples <= 0 || info.channels < 1 || info.hz <= 0) {
            continue;
        }
        if (rate == 0) {
            rate = info.hz;
            channels = info.channels > 2 ? 2 : info.channels;
        }
        const int got = samples;
        if (out_frames(frames + got, rate) > kPoolMaxFrames) {
            set_reason(reason, reasonLen, "longer than 30 minutes");
            return DecodeStatus::Duration;
        }
        const size_t base = interleaved.size();
        interleaved.resize(base + static_cast<size_t>(got) * static_cast<size_t>(channels));
        for (int i = 0; i < got; ++i) {
            interleaved[base + static_cast<size_t>(i * channels)] =
                static_cast<float>(pcm[i * info.channels]) / 32768.f;
            if (channels == 2) {
                interleaved[base + static_cast<size_t>(i * channels + 1)] =
                    static_cast<float>(pcm[i * info.channels + 1]) / 32768.f;
            }
        }
        frames += got;
    }
    if (frames == 0 || rate <= 0) {
        set_reason(reason, reasonLen, "need wav, aif, flac, or mp3");
        return DecodeStatus::Format;
    }
    out.rate = rate;
    take_pcm(out, interleaved.data(), frames, channels);
    return DecodeStatus::Ok;
}

}  // namespace

DecodeStatus decode_file(const char* path, Decoded& out, char* reason, int reasonLen) {
    out = Decoded{};
    if (path == nullptr) {
        return DecodeStatus::Io;
    }
    std::ifstream probe(path, std::ios::binary);
    if (!probe) {
        set_reason(reason, reasonLen, "could not read the file");
        return DecodeStatus::Io;
    }
    probe.seekg(0, std::ios::end);
    const std::streamoff bytes = probe.tellg();
    probe.close();
    if (bytes < 0) {
        return DecodeStatus::Io;
    }
    if (static_cast<uint64_t>(bytes) >= kPoolCapBytes) {
        set_reason(reason, reasonLen, "past 2 GB");
        return DecodeStatus::Io;
    }
    std::vector<unsigned char> data;
    if (!read_all(path, data)) {
        set_reason(reason, reasonLen, "could not read the file");
        return DecodeStatus::Io;
    }
    if (data.size() >= 12 && std::memcmp(data.data(), "RIFF", 4) == 0) {
        return decode_wav(data, out, reason, reasonLen);
    }
    if (data.size() >= 12 && std::memcmp(data.data(), "FORM", 4) == 0) {
        return decode_aiff(data, out, reason, reasonLen);
    }
    if (data.size() >= 4 && std::memcmp(data.data(), "fLaC", 4) == 0) {
        return decode_flac(data, out, reason, reasonLen);
    }
    return decode_mp3(data, out, reason, reasonLen);
}

bool write_stereo_wav(const char* path, const float* left, const float* right, int frames) {
    if (path == nullptr || left == nullptr || right == nullptr || frames < 0) {
        return false;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    const uint32_t dataBytes = static_cast<uint32_t>(frames) * 8u;
    auto u32 = [&](uint32_t v) {
        const unsigned char b[4] = {
            static_cast<unsigned char>(v & 255u),
            static_cast<unsigned char>((v >> 8) & 255u),
            static_cast<unsigned char>((v >> 16) & 255u),
            static_cast<unsigned char>((v >> 24) & 255u),
        };
        out.write(reinterpret_cast<const char*>(b), 4);
    };
    auto u16 = [&](uint16_t v) {
        const unsigned char b[2] = {
            static_cast<unsigned char>(v & 255u),
            static_cast<unsigned char>((v >> 8) & 255u),
        };
        out.write(reinterpret_cast<const char*>(b), 2);
    };
    out.write("RIFF", 4);
    u32(36u + dataBytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    u32(16);
    u16(3);
    u16(2);
    u32(static_cast<uint32_t>(kSampleRate));
    u32(static_cast<uint32_t>(kSampleRate) * 8u);
    u16(8);
    u16(32);
    out.write("data", 4);
    u32(dataBytes);
    for (int i = 0; i < frames; ++i) {
        out.write(reinterpret_cast<const char*>(left + i), 4);
        out.write(reinterpret_cast<const char*>(right + i), 4);
    }
    return static_cast<bool>(out);
}

bool read_stereo_wav(const char* path, float* left, float* right, int frames) {
    Decoded decoded;
    char reason[8];
    if (decode_file(path, decoded, reason, 8) != DecodeStatus::Ok) {
        return false;
    }
    if (decoded.rate != kSampleRate || static_cast<int>(decoded.left.size()) < frames) {
        return false;
    }
    for (int i = 0; i < frames; ++i) {
        left[i] = decoded.left[static_cast<size_t>(i)];
        right[i] = decoded.right[static_cast<size_t>(i)];
    }
    return true;
}
