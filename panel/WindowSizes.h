// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

// The fixed window sizes (no free resizing): a percentage of the panel's
// native 1432 x 996 (the prototype's page at scale 1). The choice is kept in
// the user's settings and shared by the standalone and the plug-in editor.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>

namespace lift {

struct WindowSizes {
    static constexpr int kCount = 5;
    static constexpr int kPercent[kCount] = {60, 75, 85, 100, 125};
    static constexpr int kDefault = 3;  // 100 %

    static juce::Point<int> size(int idx, int nativeW, int nativeH) {
        const float s = static_cast<float>(kPercent[juce::jlimit(0, kCount - 1, idx)]) / 100.f;
        return {juce::roundToInt(static_cast<float>(nativeW) * s), juce::roundToInt(static_cast<float>(nativeH) * s)};
    }

    static juce::PropertiesFile::Options options() {
        juce::PropertiesFile::Options o;
        o.applicationName = "LIFT";
        o.filenameSuffix = ".ui.settings";
        o.osxLibrarySubFolder = "Application Support";
        o.folderName = "LIFT";
        return o;
    }

    // The saved size; with none saved, 100 % if it fits the main display,
    // else the largest that does.
    static int load(int nativeW, int nativeH) {
        juce::PropertiesFile f(options());
        const int saved = f.getIntValue("windowSize", -1);
        if (saved >= 0 && saved < kCount) {
            return saved;
        }
        int best = 0;
        if (const auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()) {
            const auto area = d->userArea;
            for (int i = 0; i <= kDefault; ++i) {
                const auto s = size(i, nativeW, nativeH);
                if (s.x <= area.getWidth() && s.y + 40 <= area.getHeight()) {
                    best = i;
                }
            }
            return best;
        }
        return kDefault;
    }

    static void save(int idx) {
        juce::PropertiesFile f(options());
        f.setValue("windowSize", idx);
        f.saveIfNeeded();
    }
};

}  // namespace lift
