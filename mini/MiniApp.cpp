// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
//
// LIFT Mini: a separate standalone for the Mini (Raspberry Pi 4/5 target).
// The full LIFT engine (LiftProcessor) with the Mini's 480 x 272 screen and
// its 38 controls (24 keys, 10 buttons, 4 push encoders).
//
//   LIFT_Mini [--kiosk] [--fps N] [--size WxH] [--audio ALSA|JACK]
//             [--device NAME] [--buffer N] [--gpio FILE] [--screenshot FILE.png [--view radio]]
#include "MiniUi.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace {
using namespace juce;

String arg(const StringArray& a, const String& key, const String& def = {}) {
    const int i = a.indexOf(key);
    return (i >= 0 && i + 1 < a.size()) ? a[i + 1] : def;
}

// Panel MIDI (from the Pico) is taken here; everything else goes to the engine.
struct MidiSplit : MidiInputCallback {
    AudioProcessorPlayer& player;
    mini::Sink sink;
    MidiSplit(AudioProcessorPlayer& p, mini::Sink s) : player(p), sink(std::move(s)) {}
    void handleIncomingMidiMessage(MidiInput* src, const MidiMessage& m) override {
        if (!mini::midiPanelEvent(m, sink)) player.handleIncomingMidiMessage(src, m);
    }
};

class Window : public DocumentWindow {
public:
    Window(Component* c, bool kiosk, int w, int h)
        : DocumentWindow("LIFT Mini", Colours::black, kiosk ? 0 : DocumentWindow::closeButton) {
        setUsingNativeTitleBar(!kiosk);
        setContentNonOwned(c, false);
        c->setSize(w, h);
        if (kiosk) {
            setTitleBarHeight(0);
            Desktop::getInstance().setKioskModeComponent(this, false);
        } else {
            centreWithSize(w, h);
        }
        setVisible(true);
        c->grabKeyboardFocus();
    }
    void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class MiniApp : public JUCEApplication {
public:
    const String getApplicationName() override { return "LIFT Mini"; }
    const String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise(const String& cmd) override {
        const auto a = StringArray::fromTokens(cmd, true);
        proc_ = std::make_unique<lift::LiftProcessor>();
        ui_ = std::make_unique<mini::MiniUi>(*proc_, arg(a, "--fps", "30").getIntValue());
        auto sink = [this](const mini::Event& e) { if (ui_) ui_->handle(e); };

        int w = mini::kW * 2, h = mini::kH * 2;
        if (auto s = arg(a, "--size"); s.containsChar('x')) {
            w = s.upToFirstOccurrenceOf("x", false, false).getIntValue();
            h = s.fromFirstOccurrenceOf("x", false, false).getIntValue();
        }

        if (auto shot = arg(a, "--screenshot"); shot.isNotEmpty()) {
            // headless check: run the engine offline a moment, draw one frame, save it
            proc_->prepareToPlay(48000.0, 256);
            ui_->newProject();
            ui_->setSize(w, h);
            if (arg(a, "--view") == "radio") {  // SHIFT + REC, pick RADIO, open it; let the stream start
                ui_->handle({mini::Event::Button, int(mini::Btn::Shift), 1});
                ui_->handle({mini::Event::Button, int(mini::Btn::Rec), 1});
                ui_->handle({mini::Event::Button, int(mini::Btn::Shift), 0});
                ui_->handle({mini::Event::Turn, 0, 1});
                ui_->handle({mini::Event::Push, 0, 1});
                Thread::sleep(4000);
            } else {
                ui_->handle({mini::Event::Turn, 0, 4});
                ui_->handle({mini::Event::Turn, 2, -6});
            }
            auto img = ui_->createComponentSnapshot(ui_->getLocalBounds(), true, 1.f);
            File f(File::getCurrentWorkingDirectory().getChildFile(shot));
            f.deleteFile();
            FileOutputStream os(f);
            PNGImageFormat().writeImageToStream(img, os);
            os.flush();
            std::printf("wrote %s (%dx%d)\n", f.getFullPathName().toRawUTF8(), img.getWidth(), img.getHeight());
            setApplicationReturnValue(os.getStatus().wasOk() ? 0 : 1);
            quit();
            return;
        }

        // audio: ALSA by default on the Pi (WM8960 HAT), JACK if asked
        const auto type = arg(a, "--audio", "ALSA");
        devices_.initialise(2, 2, nullptr, true);
        if (devices_.getCurrentAudioDeviceType() != type) devices_.setCurrentAudioDeviceType(type, true);
        auto setup = devices_.getAudioDeviceSetup();
        setup.sampleRate = 48000.0;   // the tape rate: no resampling
        setup.bufferSize = arg(a, "--buffer", "256").getIntValue();
        if (auto d = arg(a, "--device"); d.isNotEmpty()) setup.outputDeviceName = setup.inputDeviceName = d;
        if (auto err = devices_.setAudioDeviceSetup(setup, true); err.isNotEmpty())
            ui_->setStatus("AUDIO: " + err);
        player_.setProcessor(proc_.get());
        devices_.addAudioCallback(&player_);

        // MIDI: every input (the Pico panel and any keyboard), first output gets clock + notes
        split_ = std::make_unique<MidiSplit>(player_, sink);
        for (auto& in : MidiInput::getAvailableDevices()) {
            devices_.setMidiInputDeviceEnabled(in.identifier, true);
            devices_.addMidiInputDeviceCallback(in.identifier, split_.get());
        }
        if (auto outs = MidiOutput::getAvailableDevices(); !outs.isEmpty()) {
            devices_.setDefaultMidiOutputDevice(outs[0].identifier);
            player_.setMidiOutput(devices_.getDefaultMidiOutput());
        }

        gpio_ = std::make_unique<mini::Gpio>(sink);
        const auto gpioFile = arg(a, "--gpio", "/etc/lift-mini/controls.json");
        if (auto why = gpio_->start(File(gpioFile)); why.isNotEmpty()) DBG("GPIO off: " << why);

        ui_->boot();   // the last project where it was left, else a new one
        window_ = std::make_unique<Window>(ui_.get(), a.contains("--kiosk"), w, h);
        if (a.contains("--kiosk")) Desktop::getInstance().setScreenSaverEnabled(false);
    }

    void shutdown() override {
        if (ui_ != nullptr && proc_ != nullptr) ui_->saveNow();
        gpio_.reset();
        devices_.removeAudioCallback(&player_);
        devices_.closeAudioDevice();
        player_.setProcessor(nullptr);
        window_.reset();
        ui_.reset();
        split_.reset();
        proc_.reset();
    }
    void systemRequestedQuit() override { quit(); }

private:
    std::unique_ptr<lift::LiftProcessor> proc_;
    std::unique_ptr<mini::MiniUi> ui_;
    std::unique_ptr<Window> window_;
    std::unique_ptr<MidiSplit> split_;
    std::unique_ptr<mini::Gpio> gpio_;
    AudioDeviceManager devices_;
    AudioProcessorPlayer player_;
};
}  // namespace

START_JUCE_APPLICATION(MiniApp)
