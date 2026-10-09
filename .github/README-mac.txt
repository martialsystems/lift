LIFT for macOS (unsigned test build)

LIFT.app        the standalone app
LIFT.vst3       VST3 plug-in
LIFT.component  Audio Unit plug-in

Opening the app the first time
macOS blocks apps from unidentified developers. Either:
  - Right-click (or Control-click) LIFT.app, choose Open, then Open again; or
  - In Terminal:  xattr -dr com.apple.quarantine /path/to/LIFT.app
On recent macOS you may instead need System Settings > Privacy & Security >
"Open Anyway" after the first attempt.

Installing the plug-ins
  VST3: copy LIFT.vst3 to ~/Library/Audio/Plug-Ins/VST3/
  AU:   copy LIFT.component to ~/Library/Audio/Plug-Ins/Components/
Then clear quarantine on them too, e.g.
  xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/LIFT.vst3
and rescan plug-ins in your DAW.

Audio and MIDI devices: click "Options" at the top-left of the app window.
Saves live in ~/Library/Application Support/Martial Systems/LIFT/.
