# MIDI and save slots (JUCE app)

## MIDI in

| Message | What it does |
|---|---|
| Note on / off | Plays the placeholder voice (mono, last-note priority, legato back to a held note). Velocity sets the level. |
| Pitch bend | +-2 semitones |
| CC 1 (mod wheel) | Vibrato depth (5.5 Hz, up to +-0.5 semitone) |
| CC 64 (sustain) | Holds the note until the pedal lifts |
| CC 120 / 123 | All sound off / all notes off |
| CC 0 (bank MSB) + program change | Loads save slot `bank * 128 + program + 1` (program 0 = slot 001, program 127 = slot 128, bank 1 program 0 = slot 129 ... up to 999). An empty slot is ignored. CC 32 is ignored. |
| MIDI clock (24 ppqn) | Sets the tempo (status bar number turns white while following). Without clock for 0.5 s the internal tempo returns. |
| Start | Tape to the loop start (or the top with the loop off) and play |
| Continue | Play from where the tape is |
| Stop | Tape stop (with the normal stop ramp) |
| Song position | Moves the playhead while stopped (beats at the current tempo) |

Knob CCs (value 0..127 = knob 0..1):

| CC | Knobs |
|---|---|
| 20-23 | Knobs 1-4 of the screen on show |
| 24-27 | TAPE: SPEED, BIAS, REC LVL, SCRUB |
| 28-31 | SYNTH: the four engine macros |
| 102-105 | DRUM: SLICE, PITCH, CHOKE, DECAY |
| 106-109 | MIX: LEVEL, PAN, LOW, HIGH |
| 110-113 | IN: STATION, URL, GAIN, THRESH |

### MIDI learn

SHIFT + REV turns learn on. Touch (turn) a knob, then move a controller: that
CC now drives that knob (one CC per knob; a learned CC overrides the fixed map
above). SHIFT + REV again turns learn off. CCs 0, 1, 32, 64 and 120-127 are
never learned. The learn map is part of the saved state.

## MIDI out

- MIDI clock (24 ppqn at the current tempo) while the tape runs, with Start
  and Stop when the transport starts and stops. Not sent while LIFT is
  following an incoming clock.
- Notes played on the panel keyboard (channel 1). Incoming MIDI is not echoed.

Standalone: the Options button (top left) opens JUCE's audio/MIDI settings:
audio device, sample rate, buffer size, MIDI inputs and the MIDI output.

## Save slots

- SHIFT + DROP: save. SHIFT + PLAY: load. The screen shows the slot picker:
  OCT- / OCT+, any knob, the mouse wheel, arrow keys or typed digits pick
  001-999; PLAY or Enter confirms; STOP or Esc cancels.
- A slot keeps everything: the four tracks' tape audio, the LIFT clip, the
  playhead, loop points and markers, knobs on every screen, the screen and
  pad states, arm and mutes, the shift-layer settings (octave, transpose,
  steps, length, swing, record source, character), the patch bay (cables,
  colours, stackables), synth engine, drum kit and the MIDI learn map.
- Files: `<user app data>/Martial Systems/LIFT/slot-NNN.lift`, plus
  `last-slot.txt`. macOS: `~/Library/Application Support/Martial Systems/LIFT`.
  Windows: `%APPDATA%\Martial Systems\LIFT`. Linux: `~/.config/Martial Systems/LIFT`.
- Writes go to a temporary file that is then renamed over the slot. The file
  starts with `LIFTSLOT` and a format version; a newer version is refused.
- No autosave. The standalone reopens the last slot saved or loaded on
  launch. In a DAW the host saves the same data with the project.
- Tape audio is stored at 48 kHz (the tape rate). A save made at another rate
  is resampled on load.
