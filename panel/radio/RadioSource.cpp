// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
#include "RadioSource.h"

#include "engine/resample.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lift {
using namespace juce;

// ------------------------------------------------------------------ list

File RadioList::defaultFile() {
    return File::getSpecialLocation(File::userApplicationDataDirectory)
        .getChildFile("Martial Systems").getChildFile("LIFT").getChildFile("radio.json");
}

RadioList RadioList::defaults() {
    // Public test streams (MP3, 128 kbps). Whether to ship any default
    // station is a licensing decision for Martial Systems; edit radio.json.
    RadioList l;
    l.stations = {{"SomaFM Groove Salad", "https://ice1.somafm.com/groovesalad-128-mp3"},
                  {"SomaFM Drone Zone", "https://ice1.somafm.com/dronezone-128-mp3"},
                  {"SomaFM Secret Agent", "https://ice1.somafm.com/secretagent-128-mp3"},
                  {"SomaFM DEF CON Radio", "https://ice1.somafm.com/defcon-128-mp3"},
                  {"Radio Paradise", "https://stream.radioparadise.com/mp3-128"}};
    l.presets = {0, 1, 2, 3, 4, -1, -1, -1};
    return l;
}

RadioList RadioList::load(const File& f) {
    if (!f.existsAsFile()) {
        auto d = defaults();
        d.save(f);
        return d;
    }
    RadioList l;
    const auto v = JSON::parse(f);
    const auto st = v.getProperty("stations", {});
    if (auto* a = st.getArray())
        for (const auto& s : *a) {
            RadioStation r{s.getProperty("name", "").toString(), s.getProperty("url", "").toString()};
            if (r.url.startsWithIgnoreCase("http")) l.stations.push_back(r);
        }
    const auto pr = v.getProperty("presets", {});
    if (auto* a = pr.getArray())
        for (const auto& p : *a) l.presets.push_back(int(p));
    l.presets.resize(8, -1);
    for (auto& p : l.presets)
        if (p >= int(l.stations.size())) p = -1;
    if (l.stations.empty()) return defaults();
    return l;
}

bool RadioList::save(const File& f) const {
    Array<var> st, pr;
    for (auto& s : stations) {
        auto* o = new DynamicObject();
        o->setProperty("name", s.name);
        o->setProperty("url", s.url);
        st.add(var(o));
    }
    for (int p : presets) pr.add(p);
    auto* root = new DynamicObject();
    root->setProperty("stations", st);
    root->setProperty("presets", pr);
    f.getParentDirectory().createDirectory();
    return f.replaceWithText(JSON::toString(var(root)));
}

// ------------------------------------------------------------------ source

namespace {
// The MP3 reader wants a length and scans a little ahead then seeks back.
// A live stream has neither: report a large length, and keep a 256 KB window
// so the short seeks back land in it.
// Network reads can return short; the decoder expects full reads.
class FullReads final : public InputStream {
public:
    explicit FullReads(std::unique_ptr<InputStream> s) : s_(std::move(s)) {}
    int64 getTotalLength() override { return -1; }
    bool isExhausted() override { return s_->isExhausted(); }
    int read(void* d, int n) override {
        int got = 0;
        while (got < n) {
            const int k = s_->read(static_cast<char*>(d) + got, n - got);
            if (k <= 0) break;
            got += k;
        }
        pos_ += got;
        return got;
    }
    int64 getPosition() override { return pos_; }
    bool setPosition(int64 p) override { return p == pos_; }

private:
    std::unique_ptr<InputStream> s_;
    int64 pos_ = 0;
};

class LiveStream final : public InputStream {
public:
    explicit LiveStream(std::unique_ptr<InputStream> s) : src_(std::move(s)) {}
    int64 getTotalLength() override { return int64(1) << 30; }  // big, but the reader's frame count must fit an int
    bool isExhausted() override { return pos_ >= end() && src_.isExhausted(); }
    int read(void* d, int n) override {
        if (pos_ + n > end()) {  // fetch what is missing
            const int need = static_cast<int>(pos_ + n - end());
            const size_t old = win_.size();
            win_.resize(old + size_t(need));
            const int got = src_.read(win_.data() + old, need);
            win_.resize(old + size_t(juce::jmax(0, got)));
            if (win_.size() > kKeep * 2) {  // drop the oldest, keep kKeep for seeks back
                const size_t drop = win_.size() - kKeep;
                win_.erase(win_.begin(), win_.begin() + std::ptrdiff_t(drop));
                start_ += int64(drop);
            }
        }
        const int k = static_cast<int>(juce::jlimit<int64>(0, n, end() - pos_));
        if (k > 0) std::memcpy(d, win_.data() + (pos_ - start_), size_t(k));
        pos_ += k;
        return k;
    }
    int64 getPosition() override { return pos_; }
    bool setPosition(int64 p) override {
        if (p < start_) return false;
        if (p > end()) {  // forward: read and discard
            std::vector<char> tmp(size_t(p - end()));
            pos_ = end();
            read(tmp.data(), int(tmp.size()));
            return pos_ == p;
        }
        pos_ = p;
        return true;
    }

private:
    static constexpr size_t kKeep = 1 << 18;  // 256 KB window for the decoder's seeks back
    int64 end() const { return start_ + int64(win_.size()); }
    FullReads src_;
    std::vector<char> win_;
    int64 start_ = 0, pos_ = 0;
};
}  // namespace

InternetRadioSource::InternetRadioSource(RadioList list) : Thread("LIFT radio"), list_(std::move(list)) {}

InternetRadioSource::~InternetRadioSource() {
    want_.store(-1);
    signalThreadShouldExit();
    notify();
    stopThread(10000);
}

void InternetRadioSource::tune(int s) {
    if (s >= int(list_.stations.size())) s = -1;
    want_.store(s);
    flush_.store(true);
    if (s >= 0 && !isThreadRunning()) startThread(Priority::normal);
    notify();
}

String InternetRadioSource::status() const {
    const ScopedLock l(statusLock_);
    return status_;
}

void InternetRadioSource::setStatus(const String& s) {
    const ScopedLock l(statusLock_);
    status_ = s;
}

int InternetRadioSource::read(float* l, float* r, int n) noexcept {
    if (flush_.exchange(false)) {
        fifo_.read(fifo_.getNumReady());  // drop the old station (reader side only)
        playing_ = false;
    }
    const int ready = fifo_.getNumReady();
    if (!playing_ && ready >= kPreroll) playing_ = true;
    int got = 0;
    if (playing_) {
        got = std::min(n, ready);
        const auto sc = fifo_.read(got);
        std::copy_n(ringL_.data() + sc.startIndex1, sc.blockSize1, l);
        std::copy_n(ringR_.data() + sc.startIndex1, sc.blockSize1, r);
        std::copy_n(ringL_.data() + sc.startIndex2, sc.blockSize2, l + sc.blockSize1);
        std::copy_n(ringR_.data() + sc.startIndex2, sc.blockSize2, r + sc.blockSize1);
        if (got < n) {
            playing_ = false;  // ran dry: wait for the preroll again
            underruns_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    for (int i = got; i < n; ++i) l[i] = r[i] = 0.f;
    const float lv = level.load(std::memory_order_relaxed), mo = mono.load(std::memory_order_relaxed);
    const float target = got > 0 ? 1.f : 0.f;
    for (int i = 0; i < n; ++i) {
        fade_ += (target - fade_) * 0.002f;  // ~10 ms
        const float m = 0.5f * (l[i] + r[i]);
        l[i] = (l[i] + mo * (m - l[i])) * lv * fade_;
        r[i] = (r[i] + mo * (m - r[i])) * lv * fade_;
    }
    return got;
}

void InternetRadioSource::push(const float* l, const float* r, int n) {
    // level normalisation: slow (3 s) mean square toward the keep target
    const double a = 1.0 - std::exp(-1.0 / (3.0 * 48000.0));
    std::vector<float> L(l, l + n), R(r, r + n);
    if (ms_ <= 0.0) {  // first audio: start the meter at its level, not at silence
        for (int i = 0; i < n; ++i) ms_ += 0.5 * (double(l[i]) * l[i] + double(r[i]) * r[i]) / n;
        const double db = 10.0 * std::log10(ms_ + 1e-12);
        gain_ = static_cast<float>(std::pow(10.0, jlimit(-12.0, 12.0, eng::kTargetLufs - db) / 20.0));
    }
    for (int i = 0; i < n; ++i) {
        ms_ += a * (0.5 * (double(l[i]) * l[i] + double(r[i]) * r[i]) - ms_);
        if ((i & 255) == 0) {
            const double db = 10.0 * std::log10(ms_ + 1e-12);
            const float want = static_cast<float>(std::pow(10.0, jlimit(-12.0, 12.0, eng::kTargetLufs - db) / 20.0));
            gain_ += 0.05f * (want - gain_);
        }
        L[size_t(i)] = jlimit(-1.f, 1.f, l[i] * gain_);
        R[size_t(i)] = jlimit(-1.f, 1.f, r[i] * gain_);
    }
    int done = 0;
    while (done < n && !threadShouldExit() && !flush_.load()) {
        const int free = fifo_.getFreeSpace();
        if (free == 0) {
            wait(20);  // the ring is full: real time pace
            continue;
        }
        const int k = std::min(free, n - done);
        const auto sc = fifo_.write(k);
        std::copy_n(L.data() + done, sc.blockSize1, ringL_.data() + sc.startIndex1);
        std::copy_n(R.data() + done, sc.blockSize1, ringR_.data() + sc.startIndex1);
        std::copy_n(L.data() + done + sc.blockSize1, sc.blockSize2, ringL_.data() + sc.startIndex2);
        std::copy_n(R.data() + done + sc.blockSize1, sc.blockSize2, ringR_.data() + sc.startIndex2);
        done += k;
    }
}

bool InternetRadioSource::playStream(int s) {
    const auto& st = list_.stations[size_t(s)];
    setStatus("CONNECTING " + st.name.toUpperCase());
    int statusCode = 0;
    auto in = URL(st.url).createInputStream(URL::InputStreamOptions(URL::ParameterHandling::inAddress)
                                                .withConnectionTimeoutMs(8000)
                                                .withStatusCode(&statusCode)
                                                .withExtraHeaders("User-Agent: LIFT/0.1\r\n")
                                                .withNumRedirectsToFollow(5));
    if (in == nullptr || (statusCode != 0 && statusCode >= 400)) {
        lastError_ = statusCode == 0 ? String("NO CONNECTION") : "HTTP " + String(statusCode);
        return false;
    }
    auto* live = new LiveStream(std::move(in));
    MP3AudioFormat mp3;
    std::unique_ptr<AudioFormatReader> reader(mp3.createReaderFor(live, true));
    if (reader == nullptr) {
        lastError_ = "NOT AN MP3 STREAM";
        return false;
    }
    setStatus(st.name.toUpperCase());
    ms_ = 0.0;
    const double ratio = reader->sampleRate / 48000.0;
    LagrangeInterpolator interp[2];
    constexpr int kIn = 1152 * 2;
    AudioBuffer<float> dec(2, kIn);
    std::vector<float> outL(size_t(kIn / ratio) + 64), outR(outL.size());
    int64 pos = 0;
    while (!threadShouldExit() && want_.load() == s) {
        int* dst[2] = {reinterpret_cast<int*>(dec.getWritePointer(0)), reinterpret_cast<int*>(dec.getWritePointer(1))};
        if (!reader->readSamples(dst, 2, 0, pos, kIn) || live->isExhausted()) {  // signal lost
            lastError_ = "STREAM ENDED";
            return false;
        }
        pos += kIn;
        if (reader->numChannels < 2) dec.copyFrom(1, 0, dec, 0, 0, kIn);
        // to 48 kHz
        int made = kIn;
        if (std::abs(ratio - 1.0) > 1e-6) {
            made = static_cast<int>(kIn / ratio);
            interp[0].process(ratio, dec.getReadPointer(0), outL.data(), made);
            interp[1].process(ratio, dec.getReadPointer(1), outR.data(), made);
            push(outL.data(), outR.data(), made);
        } else {
            push(dec.getReadPointer(0), dec.getReadPointer(1), kIn);
        }
    }
    return true;
}

void InternetRadioSource::run() {
    int backoff = 1;
    while (!threadShouldExit()) {
        const int s = want_.load();
        if (s < 0) {
            setStatus("OFF");
            wait(-1);
            continue;
        }
        if (playStream(s)) {
            backoff = 1;
            continue;  // retuned (or stopping)
        }
        if (threadShouldExit() || want_.load() != s) continue;
        // network loss: the audio side plays silence; say so and retry
        setStatus("NO SIGNAL (" + lastError_ + "): " + list_.stations[size_t(s)].name.toUpperCase() + ", RETRY IN " + String(backoff) + " S");
        wait(backoff * 1000);
        backoff = std::min(backoff * 2, 30);
    }
}

}  // namespace lift
