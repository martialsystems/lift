// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
//
// RADIO as a source. RadioSource is what the engine sees: tune a preset on
// the message thread, read frames on the audio thread (never blocks, never
// allocates; silence when there is nothing to play). InternetRadioSource is
// the first implementation (MP3 streams over HTTP/HTTPS, decoded on its own
// thread by JUCE's MP3 reader). The FM tuner on the Mini hardware is a later
// implementation of the same interface.
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <vector>

namespace lift {

struct RadioStation {
    juce::String name, url;
};

// Stations and the 8 preset slots, from a JSON file the user can edit:
// { "stations": [ { "name": "...", "url": "http://..." } ], "presets": [0, 1, ...] }
struct RadioList {
    std::vector<RadioStation> stations;
    std::vector<int> presets;  // slot -> station index (-1 empty), 8 slots
    static juce::File defaultFile();               // <user app data>/Martial Systems/LIFT/radio.json
    static RadioList load(const juce::File& f);    // writes the default list if the file is missing
    static RadioList defaults();
    bool save(const juce::File& f) const;
};

class RadioSource {
public:
    virtual ~RadioSource() = default;
    // message thread
    virtual void tune(int station) = 0;   // index into stations(); -1 = off
    virtual int station() const noexcept = 0;
    virtual juce::String status() const = 0;  // "OFF", "CONNECTING ...", "<name>", "NO SIGNAL ..."
    virtual const RadioList& list() const noexcept = 0;
    std::atomic<float> level{1.f};   // 0..2 after normalisation
    std::atomic<float> mono{0.f};    // 0 stereo .. 1 mono
    // audio thread: n frames at 48 kHz into l / r. Returns the frames of
    // real audio (the rest is silence). Lock-free, no allocation.
    virtual int read(float* l, float* r, int n) noexcept = 0;
};

class InternetRadioSource final : public RadioSource, private juce::Thread {
public:
    explicit InternetRadioSource(RadioList list);
    ~InternetRadioSource() override;
    void tune(int station) override;
    int station() const noexcept override { return want_.load(); }
    juce::String status() const override;
    const RadioList& list() const noexcept override { return list_; }
    int read(float* l, float* r, int n) noexcept override;
    int underruns() const noexcept { return underruns_.load(); }
    double bufferedSeconds() const noexcept { return fifo_.getNumReady() / 48000.0; }

private:
    void run() override;
    void setStatus(const juce::String& s);
    bool playStream(int station);  // false: lost / failed (retry), true: retuned or stopping
    void push(const float* l, const float* r, int n);

    static constexpr int kRing = 48000 * 4;      // 4 s
    static constexpr int kPreroll = 48000 / 2;   // start after 0.5 s buffered
    RadioList list_;
    std::atomic<int> want_{-1};
    juce::AbstractFifo fifo_{kRing};
    std::vector<float> ringL_ = std::vector<float>(kRing), ringR_ = std::vector<float>(kRing);
    bool playing_ = false;                        // audio thread: past the preroll
    std::atomic<int> underruns_{0};
    std::atomic<bool> flush_{false};
    float fade_ = 0.f;                            // audio thread: click-free starts and stops
    mutable juce::CriticalSection statusLock_;    // message thread / decode thread only
    juce::String status_ = "OFF";
    juce::String lastError_;                      // decode thread
    // decode thread: level normalisation (slow RMS toward kTargetLufs)
    double ms_ = 0.0;
    float gain_ = 1.f;
};

}  // namespace lift
