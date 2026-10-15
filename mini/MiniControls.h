// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
//
// LIFT Mini hardware controls: one event type for the 24 keys, 10 buttons
// and 4 push encoders, whatever they come from (computer keyboard, the
// panel Pico over USB MIDI, or GPIO lines read straight from the Linux GPIO
// character device). No third-party GPIO library is used.
#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

namespace mini {

enum class Btn : int { Synth, Drum, Tape, Mix, OctDown, OctUp, Lift, Drop, Rec, PlayStop, Shift, Count };
const char* btnName(Btn b) noexcept;
Btn btnFromName(const juce::String& s) noexcept;  // Btn::Count if unknown

struct Event {
    enum Kind { Key, Button, Turn, Push } kind;
    int index;   // key 0..23, Btn, or encoder 0..3 (yellow, blue, black, red)
    int value;   // Key/Button/Push: 1 down, 0 up. Turn: detents (+/-)
};

using Sink = std::function<void(const Event&)>;  // always called on the message thread

// Computer keyboard: Z..M / S..J two octaves (Q row upper), 1-4 modes, etc.
// Returns true if the key was one of ours.
bool keyboardEvent(const juce::KeyPress& k, bool down, const Sink& sink);

// The panel Pico as a USB MIDI device (spreadsheet: "Raspberry Pi Pico 2 ...
// sends to the Pi over USB"). Channel 1 notes 48..71 = keys, channel 16
// notes 0..10 = buttons, channel 16 CC 20..23 = encoders (relative, 65 = +1,
// 63 = -1), channel 16 CC 24..27 = encoder pushes (127 down, 0 up).
// Returns true if the message was a panel message (then it is not passed on
// to the engine as a played note).
bool midiPanelEvent(const juce::MidiMessage& m, const Sink& sink);

// GPIO: lines named in a JSON file, read from /dev/gpiochipN with the
// kernel's v2 character-device ioctls (no libgpiod, no wiringPi). Polls at
// 1 kHz on its own thread, debounces buttons, decodes quadrature encoders.
class Gpio {
public:
    explicit Gpio(Sink sink);
    ~Gpio();
    // Returns an empty string on success, else a reason (the app runs without GPIO).
    juce::String start(const juce::File& json);
    void stop();

private:
    struct Line { int offset; int kind; int index; bool activeLow; };  // kind 0 button, 1 key, 2 encA, 3 encB, 4 push
    Sink sink_;
    std::vector<Line> lines_;
    int fd_ = -1;
    std::thread thread_;
    std::atomic<bool> run_{false};
    void loop();
};

}  // namespace mini
