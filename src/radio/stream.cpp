// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "radio/stream.h"

#include "minimp3.h"
#include "stb_vorbis.c"

#include <opus/opus.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr int kRate = 48000;
constexpr int kPendingCap = 8 * 1024 * 1024;

struct ByteBuf {
    std::vector<unsigned char> bytes;
    size_t off = 0;

    int size() const { return static_cast<int>(bytes.size() - off); }
    const unsigned char* data() const { return bytes.data() + off; }

    void append(const unsigned char* p, int n) {
        if (n <= 0) {
            return;
        }
        bytes.insert(bytes.end(), p, p + n);
    }

    void consume(int n) {
        if (n <= 0) {
            return;
        }
        if (n > size()) {
            n = size();
        }
        off += static_cast<size_t>(n);
        if (off > 4096 && off * 2 > bytes.size()) {
            bytes.erase(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(off));
            off = 0;
        }
    }
};

struct Resampler {
    int rate = 0;
    double pos = 0.0;
    std::vector<float> left;
    std::vector<float> right;

    void push(const float* interleaved, int frames, int channels, int inRate) {
        if (frames <= 0 || channels <= 0 || inRate <= 0) {
            return;
        }
        if (rate == 0) {
            rate = inRate;
        }
        const int useRate = rate;
        if (inRate != useRate) {
            return;
        }
        left.reserve(left.size() + static_cast<size_t>(frames));
        right.reserve(right.size() + static_cast<size_t>(frames));
        for (int i = 0; i < frames; ++i) {
            const float l = interleaved[i * channels];
            const float r = channels > 1 ? interleaved[i * channels + 1] : l;
            left.push_back(l);
            right.push_back(r);
        }
    }

    int take(float* stereo, int maxFrames) {
        if (stereo == nullptr || maxFrames <= 0 || rate <= 0) {
            return 0;
        }
        if (rate == kRate) {
            int n = static_cast<int>(left.size());
            if (n > maxFrames) {
                n = maxFrames;
            }
            for (int i = 0; i < n; ++i) {
                stereo[i * 2] = left[static_cast<size_t>(i)];
                stereo[i * 2 + 1] = right[static_cast<size_t>(i)];
            }
            left.erase(left.begin(), left.begin() + n);
            right.erase(right.begin(), right.begin() + n);
            return n;
        }
        const double step = static_cast<double>(rate) / static_cast<double>(kRate);
        int out = 0;
        while (out < maxFrames) {
            const int i0 = static_cast<int>(pos);
            if (i0 + 1 >= static_cast<int>(left.size())) {
                break;
            }
            const float t = static_cast<float>(pos - static_cast<double>(i0));
            const float l0 = left[static_cast<size_t>(i0)];
            const float l1 = left[static_cast<size_t>(i0 + 1)];
            const float r0 = right[static_cast<size_t>(i0)];
            const float r1 = right[static_cast<size_t>(i0 + 1)];
            stereo[out * 2] = l0 + (l1 - l0) * t;
            stereo[out * 2 + 1] = r0 + (r1 - r0) * t;
            pos += step;
            ++out;
        }
        int drop = static_cast<int>(pos);
        if (drop > 0) {
            if (drop > static_cast<int>(left.size())) {
                drop = static_cast<int>(left.size());
            }
            left.erase(left.begin(), left.begin() + drop);
            right.erase(right.begin(), right.begin() + drop);
            pos -= static_cast<double>(drop);
        }
        return out;
    }
};

struct Mp3Dec {
    mp3dec_t dec{};
    bool ready = false;
};

struct VorbisDec {
    stb_vorbis* handle = nullptr;
    int rate = 0;
};

struct OpusDec {
    OpusDecoder* handle = nullptr;
    int channels = 0;
    int preSkip = 0;
    int skipped = 0;
    bool head = false;
    bool tags = false;
    std::vector<unsigned char> packet;
};

enum class Kind { None, Mp3, Vorbis, Opus };

bool push_mp3(Mp3Dec& mp3, Resampler& out, ByteBuf& buf) {
    if (!mp3.ready) {
        mp3dec_init(&mp3.dec);
        mp3.ready = true;
    }
    while (buf.size() > 4) {
        short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        mp3dec_frame_info_t info{};
        // minimp3 zeros the decoder, including the bit reservoir, when the
        // buffer does not hold a full frame. Put the previous decoder back
        // and wait for the rest of that frame.
        const mp3dec_t saved = mp3.dec;
        const int samples = mp3dec_decode_frame(&mp3.dec, buf.data(), buf.size(), pcm, &info);
        const bool needMore = samples <= 0 &&
            (info.frame_bytes <= 0 || info.frame_bytes >= buf.size() || info.hz <= 0);
        if (needMore) {
            mp3.dec = saved;
            if (buf.size() > 4096 && info.frame_bytes <= 0) {
                buf.consume(1);
                continue;
            }
            break;
        }
        buf.consume(info.frame_bytes);
        if (samples <= 0 || info.channels <= 0 || info.hz <= 0) {
            continue;
        }
        std::vector<float> frame(static_cast<size_t>(samples * info.channels));
        for (int i = 0; i < samples * info.channels; ++i) {
            frame[static_cast<size_t>(i)] = static_cast<float>(pcm[i]) / 32768.f;
        }
        out.push(frame.data(), samples, info.channels, info.hz);
    }
    return buf.size() <= kPendingCap;
}

bool push_vorbis(VorbisDec& vorbis, Resampler& out, ByteBuf& buf) {
    if (vorbis.handle == nullptr) {
        int used = 0;
        int err = 0;
        vorbis.handle = stb_vorbis_open_pushdata(buf.data(), buf.size(), &used, &err, nullptr);
        if (vorbis.handle == nullptr) {
            return err == VORBIS_need_more_data && buf.size() <= kPendingCap;
        }
        buf.consume(used);
        const stb_vorbis_info info = stb_vorbis_get_info(vorbis.handle);
        vorbis.rate = static_cast<int>(info.sample_rate);
        if (vorbis.rate <= 0) {
            return false;
        }
    }
    while (buf.size() > 0) {
        int channels = 0;
        int samples = 0;
        float** audio = nullptr;
        const int used = stb_vorbis_decode_frame_pushdata(
            vorbis.handle, buf.data(), buf.size(), &channels, &audio, &samples);
        if (used <= 0) {
            break;
        }
        buf.consume(used);
        if (samples <= 0 || channels <= 0 || audio == nullptr) {
            continue;
        }
        std::vector<float> frame(static_cast<size_t>(samples * channels));
        for (int i = 0; i < samples; ++i) {
            for (int c = 0; c < channels; ++c) {
                frame[static_cast<size_t>(i * channels + c)] = audio[c][i];
            }
        }
        out.push(frame.data(), samples, channels, vorbis.rate);
    }
    return buf.size() <= kPendingCap;
}

bool opus_packet(OpusDec& opus, Resampler& out) {
    const std::vector<unsigned char>& packet = opus.packet;
    if (packet.empty()) {
        return true;
    }
    if (!opus.head) {
        if (packet.size() < 19 || std::memcmp(packet.data(), "OpusHead", 8) != 0) {
            return false;
        }
        const int version = packet[8];
        const int channels = packet[9];
        const int family = packet[18];
        if ((version != 0 && version != 1) || (channels != 1 && channels != 2) || family != 0) {
            return false;
        }
        int err = 0;
        opus.handle = opus_decoder_create(kRate, channels, &err);
        if (opus.handle == nullptr || err != OPUS_OK) {
            return false;
        }
        opus.channels = channels;
        opus.preSkip = packet[10] | (packet[11] << 8);
        opus.head = true;
        return true;
    }
    if (!opus.tags) {
        opus.tags = true;
        return true;
    }
    opus_int16 pcm[5760 * 2];
    const int got = opus_decode(
        opus.handle, packet.data(), static_cast<opus_int32>(packet.size()), pcm, 5760, 0);
    if (got <= 0) {
        return true;
    }
    int skip = 0;
    if (opus.skipped < opus.preSkip) {
        skip = opus.preSkip - opus.skipped;
        if (skip > got) {
            skip = got;
        }
        opus.skipped += skip;
    }
    const int keep = got - skip;
    if (keep <= 0) {
        return true;
    }
    std::vector<float> frame(static_cast<size_t>(keep * opus.channels));
    const opus_int16* src = pcm + skip * opus.channels;
    for (int i = 0; i < keep * opus.channels; ++i) {
        frame[static_cast<size_t>(i)] = static_cast<float>(src[i]) / 32768.f;
    }
    out.push(frame.data(), keep, opus.channels, kRate);
    return true;
}

bool push_opus(OpusDec& opus, Resampler& out, ByteBuf& buf) {
    while (buf.size() >= 4) {
        const unsigned char* p = buf.data();
        if (!(p[0] == 'O' && p[1] == 'g' && p[2] == 'g' && p[3] == 'S')) {
            int at = -1;
            const int n = buf.size();
            for (int i = 1; i + 3 < n; ++i) {
                if (p[i] == 'O' && p[i + 1] == 'g' && p[i + 2] == 'g' && p[i + 3] == 'S') {
                    at = i;
                    break;
                }
            }
            if (at < 0) {
                if (buf.size() > 3) {
                    buf.consume(buf.size() - 3);
                }
                break;
            }
            buf.consume(at);
            continue;
        }
        if (buf.size() < 27) {
            break;
        }
        const int segs = p[26];
        if (buf.size() < 27 + segs) {
            break;
        }
        int body = 0;
        for (int i = 0; i < segs; ++i) {
            body += p[27 + i];
        }
        if (buf.size() < 27 + segs + body) {
            break;
        }
        const unsigned char* table = p + 27;
        const unsigned char* data = table + segs;
        int cursor = 0;
        for (int i = 0; i < segs; ++i) {
            const int n = table[i];
            opus.packet.insert(opus.packet.end(), data + cursor, data + cursor + n);
            cursor += n;
            if (n < 255) {
                if (opus.packet.size() > 1024u * 1024u || !opus_packet(opus, out)) {
                    return false;
                }
                opus.packet.clear();
            }
        }
        buf.consume(27 + segs + body);
    }
    return opus.packet.size() <= 1024u * 1024u && buf.size() <= kPendingCap;
}

}  // namespace

struct StreamDecoder {
    Kind kind = Kind::None;
    ByteBuf pending;
    Resampler out;
    Mp3Dec mp3;
    VorbisDec vorbis;
    OpusDec opus;
};

StreamDecoder* stream_open(const char* codec) {
    if (codec == nullptr) {
        return nullptr;
    }
    auto* decoder = new StreamDecoder();
    if (std::strcmp(codec, "mp3") == 0) {
        decoder->kind = Kind::Mp3;
        return decoder;
    }
    if (std::strcmp(codec, "vorbis") == 0) {
        decoder->kind = Kind::Vorbis;
        return decoder;
    }
    if (std::strcmp(codec, "opus") == 0) {
        decoder->kind = Kind::Opus;
        return decoder;
    }
    delete decoder;
    return nullptr;
}

void stream_close(StreamDecoder* decoder) {
    if (decoder == nullptr) {
        return;
    }
    if (decoder->vorbis.handle != nullptr) {
        stb_vorbis_close(decoder->vorbis.handle);
    }
    if (decoder->opus.handle != nullptr) {
        opus_decoder_destroy(decoder->opus.handle);
    }
    delete decoder;
}

bool stream_push(StreamDecoder* decoder, const unsigned char* data, int n) {
    if (decoder == nullptr || (data == nullptr && n > 0) || n < 0) {
        return false;
    }
    decoder->pending.append(data, n);
    if (decoder->kind == Kind::Mp3) {
        return push_mp3(decoder->mp3, decoder->out, decoder->pending);
    }
    if (decoder->kind == Kind::Vorbis) {
        return push_vorbis(decoder->vorbis, decoder->out, decoder->pending);
    }
    if (decoder->kind == Kind::Opus) {
        return push_opus(decoder->opus, decoder->out, decoder->pending);
    }
    return false;
}

int stream_take(StreamDecoder* decoder, float* stereo, int maxFrames) {
    if (decoder == nullptr) {
        return 0;
    }
    return decoder->out.take(stereo, maxFrames);
}
