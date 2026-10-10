// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

// The processor's resampling side (P3), message thread: keeps from the
// always-on capture buffer, the trim stage, places for a kept clip, and the
// standalone WAV export. The audio thread only ever sees raw pointers to
// clips that live_ keeps alive. See docs/SIGNAL-ORDER.md.

#include "LiftProcessor.h"

#include "tape/transport.h"

#include <algorithm>

namespace lift {

namespace {
constexpr double kRate = 48000.0;
}

LiftProcessor::ClipPtr LiftProcessor::makeClip(std::vector<float> l, std::vector<float> r, bool loop) {
    const int n = static_cast<int>(std::min(l.size(), r.size()));
    if (n < 64) {
        return nullptr;
    }
    auto c = std::make_shared<eng::Clip>();
    c->l = std::move(l);
    c->r = std::move(r);
    c->l.resize(static_cast<size_t>(n));
    c->r.resize(static_cast<size_t>(n));
    c->lufsIn = eng::integratedLufs(c->l.data(), c->r.data(), n, kRate);
    if (c->lufsIn < -70.f) {
        return nullptr;  // nothing heard: no keep
    }
    // the automatic trim stage before every keep: loudness to target, true-peak safe
    c->gainDb = eng::trimToTarget(c->l.data(), c->r.data(), n, kRate);
    c->loop = loop;
    c->id = nextClipId_++;
    // the newest keep rides along in save slots (TapeRuntime::clip)
    TapeRuntime& rt = *rt_;
    if (rt.clip[0] != nullptr) {
        const int m = std::min(n, rt.frames);
        std::copy(c->l.begin(), c->l.begin() + m, rt.clip[0]);
        std::copy(c->r.begin(), c->r.begin() + m, rt.clip[1]);
        rt.clipFrames = m;
        uiClipFrames.store(m);
    }
    return c;
}

LiftProcessor::ClipPtr LiftProcessor::keepLast(double seconds) {
    const int n = static_cast<int>(std::clamp(seconds, 0.05, static_cast<double>(capture_.capacity()) / kRate) * kRate);
    const int have = static_cast<int>(std::min<std::int64_t>(capture_.written(), capture_.capacity()));
    const int take = std::min(n, have);
    if (take <= 0) {
        return nullptr;
    }
    std::vector<float> l(static_cast<size_t>(take)), r(static_cast<size_t>(take));
    capture_.read(l.data(), r.data(), take);
    // a keep of at least two beats counts as a loop (whole-loop default selection)
    const double beat = 60.0 / std::max(20.0, tempoBpm.load()) * kRate;
    return makeClip(std::move(l), std::move(r), take >= 2.0 * beat);
}

LiftProcessor::ClipPtr LiftProcessor::keepRange(std::int64_t from, std::int64_t to) {
    const std::int64_t now = capture_.written();
    to = std::min(to, now);
    from = std::max(from, now - capture_.capacity());
    const int take = static_cast<int>(to - from);
    if (take <= 0) {
        return nullptr;
    }
    std::vector<float> l(static_cast<size_t>(take)), r(static_cast<size_t>(take));
    capture_.read(l.data(), r.data(), take, to);
    return makeClip(std::move(l), std::move(r), true);
}

LiftProcessor::ClipPtr LiftProcessor::keepTracks() {
    TapeRuntime& rt = *rt_;
    int start = 0, end = rt.frames;
    if (rt.loopEnd > rt.loopStart) {
        start = rt.loopStart;
        end = std::min(rt.loopEnd, rt.frames);
    }
    const int n = end - start;
    if (n <= 0) {
        return nullptr;
    }
    std::vector<float> l(static_cast<size_t>(n)), r(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        float a = 0.f, b = 0.f;
        for (int t = 0; t < kTrackCount; ++t) {
            a += rt.ch[t][0][start + i];
            b += rt.ch[t][1][start + i];
        }
        l[static_cast<size_t>(i)] = a;
        r[static_cast<size_t>(i)] = b;
    }
    return makeClip(std::move(l), std::move(r), true);
}

LiftProcessor::ClipPtr LiftProcessor::slotClip() const {
    const TapeRuntime& rt = *rt_;
    if (rt.clip[0] == nullptr || rt.clipFrames < 64) {
        return nullptr;
    }
    auto c = std::make_shared<eng::Clip>();
    c->l.assign(rt.clip[0], rt.clip[0] + rt.clipFrames);
    c->r.assign(rt.clip[1], rt.clip[1] + rt.clipFrames);
    c->lufsIn = eng::integratedLufs(c->l.data(), c->r.data(), rt.clipFrames, kRate);
    c->loop = true;
    return c;
}

void LiftProcessor::keepAlive(const ClipPtr& c) {
    if (c == nullptr) {
        return;
    }
    if (std::find(live_.begin(), live_.end(), c) == live_.end()) {
        live_.push_back(c);
    }
    // forget clips nobody can reach any more: not the panel (use_count 1),
    // not a key, the keys, a slice kit, an audition or a pending drop
    if (live_.size() > 16 && pendingDrop_.load() == nullptr) {
        auto referenced = [this](const eng::Clip* p) {
            if (keysClip_.load() == p || audition_.load() == p) return true;
            for (auto& k : keyClip_) if (k.load() == p) return true;
            for (auto& kit : liveKits_) if (kit->clip.get() == p) return true;
            return false;
        };
        live_.erase(std::remove_if(live_.begin(), live_.end(),
                                   [&](const ClipPtr& x) { return x.use_count() == 1 && !referenced(x.get()); }),
                    live_.end());
    }
}

void LiftProcessor::dropToLoop(const ClipPtr& c, int track) {
    if (c == nullptr) {
        return;
    }
    keepAlive(c);
    pendingDrop_.store(c.get());
    send(Cmd::DropClip, juce::jlimit(0, kTrackCount - 1, track));
}

void LiftProcessor::setKeysClip(const ClipPtr& c) {
    keepAlive(c);
    keysClip_.store(c.get());
}

void LiftProcessor::setKeyClip(int note, const ClipPtr& c) {
    if (note < 0 || note > 127) {
        return;
    }
    keepAlive(c);
    keyClip_[note].store(c.get());
}

void LiftProcessor::setAudition(const ClipPtr& c) {
    keepAlive(c);
    audition_.store(c.get());
}

void LiftProcessor::setSliceKit(const ClipPtr& c, int slices) {
    if (c == nullptr) {
        sliceKit_.store(nullptr);
        return;
    }
    keepAlive(c);
    auto kit = std::make_shared<SliceKit>();
    kit->clip = c;
    const int n = c->frames();
    slices = juce::jlimit(1, 24, slices);
    // slice at onsets when the clip has them (a beat), else equal parts
    std::vector<int> on;
    {
        const int hop = 256;
        float prev = 0.f, peak = 1e-6f;
        for (int i = 0; i < n; ++i) peak = std::max(peak, std::abs(c->l[static_cast<size_t>(i)]));
        int last = -100000;
        for (int i = 0; i + hop <= n; i += hop) {
            float e = 0.f;
            for (int k = 0; k < hop; ++k) e = std::max(e, std::abs(c->l[static_cast<size_t>(i + k)]));
            if (e > 0.25f * peak && e > 2.f * prev && i - last > 4800) {
                on.push_back(i);
                last = i;
            }
            prev = e;
        }
    }
    if (static_cast<int>(on.size()) >= 2) {
        if (static_cast<int>(on.size()) > slices) on.resize(static_cast<size_t>(slices));
        kit->count = static_cast<int>(on.size());
        for (int k = 0; k < kit->count; ++k) kit->start[k] = on[static_cast<size_t>(k)];
    } else {
        kit->count = slices;
        for (int k = 0; k < slices; ++k) kit->start[k] = static_cast<int>(static_cast<double>(n) * k / slices);
    }
    kit->start[kit->count] = n;
    liveKits_.push_back(kit);
    sliceKit_.store(kit.get());
}

// audio thread: keys to a clip (SELECT audition, a key's one-shot, the
// newest clip across the keys) or to the synth
void LiftProcessor::playNote(int note, float vel) noexcept {
    const int nn = note & 0x7f;
    if (const eng::Clip* a = audition_.load(std::memory_order_acquire)) {
        inst_.clips.trigger(a->l.data(), a->r.data(), a->frames(), std::pow(2.0, (nn - 60) / 12.0), vel, nn, false);
        return;
    }
    if (const eng::Clip* k = keyClip_[nn].load(std::memory_order_acquire)) {
        inst_.clips.trigger(k->l.data(), k->r.data(), k->frames(), 1.0, vel, nn, true);
        return;
    }
    if (const eng::Clip* kb = keysClip_.load(std::memory_order_acquire)) {
        inst_.clips.trigger(kb->l.data(), kb->r.data(), kb->frames(), std::pow(2.0, (nn - 60) / 12.0), vel, nn, false);
        return;
    }
    inst_.noteOn(note, vel);
}

void LiftProcessor::stopNote(int note) noexcept {
    if (note < 0) {
        inst_.clips.releaseAll();
    } else {
        inst_.clips.release(note & 0x7f);
    }
    inst_.noteOff(note);
}

// ---------------------------------------------------------------- export

bool LiftProcessor::exportWavs(const juce::File& folder, const ClipPtr& clip, juce::StringArray* written) {
    if (!folder.isDirectory() && !folder.createDirectory()) {
        return false;
    }
    juce::WavAudioFormat wav;
    auto write = [&](const juce::String& name, const float* l, const float* r, int n) {
        if (n <= 0) return true;
        const juce::File f = folder.getChildFile(name);
        f.deleteFile();
        std::unique_ptr<juce::OutputStream> os(f.createOutputStream());
        if (os == nullptr) return false;
        std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), kRate, 2, 24, {}, 0));
        if (w == nullptr) return false;
        os.release();  // the writer owns it now
        const float* ch[2] = {l, r};
        const bool ok = w->writeFromFloatArrays(ch, 2, n);
        if (written != nullptr) written->add(f.getFullPathName());
        return ok;
    };
    bool ok = true;
    if (clip != nullptr) {
        ok &= write("LIFT-clip.wav", clip->l.data(), clip->r.data(), clip->frames());
    }
    TapeRuntime& rt = *rt_;
    const bool loop = rt.loopEnd > rt.loopStart;
    for (int t = 0; t < kTrackCount && t < 4; ++t) {
        int a = loop ? rt.loopStart : 0, b = loop ? std::min(rt.loopEnd, rt.frames) : rt.frames;
        if (!loop) {  // the whole tape, up to the last sound on it
            while (b > a && rt.ch[t][0][b - 1] == 0.f && rt.ch[t][1][b - 1] == 0.f) --b;
        }
        ok &= write("LIFT-T" + juce::String(t + 1) + ".wav", rt.ch[t][0] + a, rt.ch[t][1] + a, b - a);
    }
    const int n = static_cast<int>(std::min<std::int64_t>(capture_.written(), capture_.capacity()));
    if (n > 0) {
        std::vector<float> l(static_cast<size_t>(n)), r(static_cast<size_t>(n));
        capture_.read(l.data(), r.data(), n);
        ok &= write("LIFT-master.wav", l.data(), r.data(), n);
    }
    return ok;
}

}  // namespace lift
