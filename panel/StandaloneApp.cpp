// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

// LIFT standalone: JUCE's standalone window (audio and MIDI settings, state
// save/load) with the fixed window sizes added to its Options menu. The app
// class follows JIDAI RACK's custom standalone (jidai-collection,
// jidai-rack/plugin/StandaloneApp.cpp: JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP).

#include "LiftEditor.h"

#include <juce_audio_plugin_client/detail/juce_CreatePluginFilter.h>
#include <juce_audio_utils/juce_audio_utils.h>

#if JucePlugin_Build_Standalone && JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP

#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

namespace {

class LiftWindow : public juce::StandaloneFilterWindow {
public:
    using juce::StandaloneFilterWindow::StandaloneFilterWindow;

private:
    lift::LiftEditor* editor() {
        auto* h = getPluginHolder();
        return h != nullptr && h->processor != nullptr ? dynamic_cast<lift::LiftEditor*>(h->processor->getActiveEditor())
                                                       : nullptr;
    }
    void buttonClicked(juce::Button* button) override {
        juce::PopupMenu m;
        juce::Component::SafePointer<LiftWindow> self(this);
        auto act = [self](int r) {
            return [self, r] {
                if (self != nullptr) {
                    self->handleMenuResult(r);
                }
            };
        };
        m.addItem("Audio/MIDI Settings...", act(1));
        m.addSeparator();
        if (auto* e = editor()) {
            e->addSizeItems(m);
            m.addSeparator();
        }
        m.addItem("Save current state...", act(2));
        m.addItem("Load a saved state...", act(3));
        m.addSeparator();
        m.addItem("Reset to default state", act(4));
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(button));
    }
};

class LiftStandaloneApp : public juce::JUCEApplication {
public:
    LiftStandaloneApp() {
        juce::PropertiesFile::Options o;
        o.applicationName = "LIFT";
        o.filenameSuffix = ".settings";
        o.osxLibrarySubFolder = "Application Support";
#if JUCE_LINUX || JUCE_BSD
        o.folderName = "~/.config";
#endif
        props_.setStorageParameters(o);
    }
    const juce::String getApplicationName() override { return "LIFT"; }
    const juce::String getApplicationVersion() override { return JucePlugin_VersionString; }
    bool moreThanOneInstanceAllowed() override { return true; }
    void anotherInstanceStarted(const juce::String&) override {}

    void initialise(const juce::String&) override {
        auto holder = std::make_unique<juce::StandalonePluginHolder>(props_.getUserSettings(), false, juce::String{}, nullptr,
                                                                      juce::Array<juce::StandalonePluginHolder::PluginInOuts>{}, false);
        window_ = std::make_unique<LiftWindow>(
            getApplicationName(), juce::LookAndFeel::getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId),
            std::move(holder));
        window_->setVisible(true);
    }
    void shutdown() override {
        window_ = nullptr;
        props_.saveIfNeeded();
    }
    void systemRequestedQuit() override {
        if (window_ != nullptr) {
            window_->pluginHolder->savePluginState();
        }
        if (juce::ModalComponentManager::getInstance()->cancelAllModalComponents()) {
            juce::Timer::callAfterDelay(100, [] {
                if (auto* app = juce::JUCEApplicationBase::getInstance()) {
                    app->systemRequestedQuit();
                }
            });
        } else {
            quit();
        }
    }

private:
    juce::ApplicationProperties props_;
    std::unique_ptr<LiftWindow> window_;
};

}  // namespace

juce::JUCEApplicationBase* juce_CreateApplication();
juce::JUCEApplicationBase* juce_CreateApplication() {
    return new LiftStandaloneApp();
}

#endif
