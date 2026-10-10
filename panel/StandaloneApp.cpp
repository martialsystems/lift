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
    void handleMenuResult(int r) {
        if (r == 102) {
            saveAs();
        } else if (r == 103) {
            open();
        } else if (r == 104) {
            exportWavs();
        } else {
            juce::StandaloneFilterWindow::handleMenuResult(r);
        }
    }
    lift::LiftProcessor* proc() {
        auto* h = getPluginHolder();
        return h != nullptr ? dynamic_cast<lift::LiftProcessor*>(h->processor.get()) : nullptr;
    }
    static juce::File stateFolder() {
        return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("LIFT");
    }
    void saveAs() {
        stateFolder().createDirectory();
        chooser_ = std::make_unique<juce::FileChooser>("Save LIFT state", stateFolder().getChildFile("LIFT.lift"), "*.lift");
        juce::Component::SafePointer<LiftWindow> self(this);
        chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                                  juce::FileBrowserComponent::warnAboutOverwriting,
                              [self](const juce::FileChooser& fc) {
                                  if (self == nullptr) return;
                                  auto f = fc.getResult();
                                  auto* p = self->proc();
                                  if (f == juce::File{} || p == nullptr) return;
                                  if (!f.hasFileExtension("lift")) f = f.withFileExtension("lift");
                                  // the state is snapshotted here; the file writes on a background thread
                                  p->saveStateToFileAsync(f, [f](bool ok) {
                                      if (!ok) {
                                          juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "LIFT",
                                                                                 "Could not write " + f.getFullPathName());
                                      }
                                  });
                              });
    }
    void open() {
        chooser_ = std::make_unique<juce::FileChooser>("Load LIFT state", stateFolder(), "*.lift");
        juce::Component::SafePointer<LiftWindow> self(this);
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [self](const juce::FileChooser& fc) {
                                  if (self == nullptr) return;
                                  const auto f = fc.getResult();
                                  auto* p = self->proc();
                                  if (!f.existsAsFile() || p == nullptr) return;
                                  juce::MemoryBlock blob;
                                  const bool read = f.loadFileAsData(blob);
                                  if (!read || p->loadState(blob.getData(), blob.getSize(), 0) != lift::LiftProcessor::LoadResult::Ok) {
                                      juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "LIFT",
                                                                             f.getFileName() + " is not a LIFT state this version can load.");
                                  }
                              });
    }
    // Export: the current clip (its selection), T1-T4 and the master (the
    // capture: the last 60 s heard) as 24-bit WAV at 48 kHz into a folder
    void exportWavs() {
        stateFolder().createDirectory();
        chooser_ = std::make_unique<juce::FileChooser>("Export WAVs to a folder", stateFolder());
        juce::Component::SafePointer<LiftWindow> self(this);
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [self](const juce::FileChooser& fc) {
                                  if (self == nullptr) return;
                                  const auto f = fc.getResult();
                                  if (f == juce::File{}) return;
                                  if (auto* e = self->editor()) e->panel().exportTo(f);
                              });
    }
    std::unique_ptr<juce::FileChooser> chooser_;

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
        // LIFT's own save / load: the stock items write getStateInformation,
        // which is empty in the standalone (no autosave of the tape there)
        m.addItem("Save current state...", act(102));
        m.addItem("Load a saved state...", act(103));
        m.addItem("Export WAVs (clip, T1-T4, master)...", act(104));
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
