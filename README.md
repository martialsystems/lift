Copyright (c) 2026 Martial Systems LLC. All rights reserved.

# LIFT

**A standalone instrument. Play a synth or a kit, print it to tape, move on.**

Inspired by the Teenage Engineering OP-1. One window. One audio device. One sample disk. One tape. Four encoders sit under the screen. A patchbay under the panel rewires the instrument. A radio dial and a drop folder feed the sample pool. The tape is four tracks of modeled analog tape, with lift, drop, reverse, and varispeed.

## Panel

The layout is fixed. Two faces share the same hit targets.

The screen is drawn for this instrument. One object per encoder. Compact shows a glyph and no parameter names. Bay shows the patch cords, with the name in small type under the cord. Encoder colors are fixed: red, blue, ochre, white.

- Synth, Drum, Tape, Mix, and In change the screen. The tape stays as printed until you record.
- Keys 1 to 8 select the engine, the kit, the tape, or the input. On Mix they are unused.
- The keyboard is two octaves.
- Lift copies the loop on the armed track. With no loop set, it copies the whole track.
- Drop writes that copy at the playhead. You choose replace or overdub. Overdub crossfades 5 ms at the edges.
- Rec with Play prints the live instrument, the input, or a resample onto the armed track.
- Rev reverses the armed track. Rev again, or Stop, plays forward.
- Bay opens the patch screen. Audio keeps running.
- Radio opens the dial on the In screen.

## Tape

Four stereo tracks. Eight tapes in a project, one loaded at a time. Each track is six minutes at 48 kHz, 32-bit float, stored on disk.

One playhead. One loop. Arm is per track. Playback runs from 0.25× to 4×. Record stays at 1×. Reverse reads the armed track backward and wraps inside the loop when a loop is set. With Stop held, encoder 4 scrubs. The metronome stays off the tape unless the input is set to resample.

The tape model runs on the way in and again on the way out, so a bounce picks up another generation. The chain is record level into a soft clip, a high shelf, a nonlinearity, bias, a low resonant bump, playback EQ, wow and flutter, and a hiss floor. Low bias dulls and compresses. High bias brightens and thins.

Four coefficient rows, one model. Deck and Pocket ship. Deck is quiet: low wow, low flutter, a mild bump, low hiss, wide bandwidth. Pocket is the dull, fluttering one: a strong bump, more hiss, bandwidth near 12 kHz. Shed has more wow and a mid bandwidth. Cap is flatter, with no bump.

Two qualities. Eco drives an asymmetric tanh. Full runs a hysteresis model, one solver per channel. A sound printed through the model four times comes back darker and more compressed. Full mode is asymmetric.

## Instruments

One instrument voice at a time, printed to tape. Synth engines are six-voice. The drum engine plays slices. Every engine loads a default patch and makes sound before the bay is opened. Four macros sit on the encoders. Anything past those four is a bay destination.

1. Loom. Two oscillators, saw and square, with detune, a filter, and an envelope.
2. Bend. A phase-distortion amount on one carrier.
3. Fold. A sine into a wavefolder, with symmetry and offset.
4. Ratio. Four sine operators, nine fixed algorithms, ratios from a table written for LIFT. Macros: algorithm, ratio set, index, feedback.
5. Wire. A plucked string, with damping and pluck position.
6. Swarm. Detuned sines through one amp envelope.
7. Spool. One file, sliced across the keys or mapped from a root key.
8. Spare. Empty in this version.

Drum mode uses one file, up to 24 slices, with choke groups for hats. Slice pitch is a bay destination. With no file loaded, Tap covers kicks and hats: two operators, no sample. Eight kit slots.

The factory bank is 64 synth presets and 16 kits, written for LIFT and stored in the project.

## Spool and radio

One pool. Drop a file on the window, or put it in the project's `in/` folder. Import decodes off the audio thread, shows progress on the In screen, then commits the file. You pick kit or Spool with encoder 1.

Wav, aif, and flac are the formats on the screen. Mp3 is accepted and stored as 48 kHz float wav. The cap is 2 GB per project, with a warning at 1.5 GB. A file already in the pool is recognized by its contents and not stored again. A 30-minute file is refused or trimmed, with the reason on screen.

The radio is a client. A directory ships with the app, and you can keep your own list. Each entry is a name, a URL, and a codec hint. Streams are Icecast or plain HTTP, in Opus, Vorbis, or MP3. Encoder 1 moves through the directory. Encoder 2 takes a URL. Encoder 3 is input gain. Encoder 4 is the record threshold. Decode runs on a side thread into a ring. If the stream underruns, the audio callback keeps running and a small flag marks the gap.

Rec in Spool captures up to 20 seconds into the pool. With a track armed, the station prints live. The radio buffer is also a bay source, hard-clipped before it reaches a voice.

You can hear an input without printing it. Tape playback is monitored unless the track is muted.

## Effects

One insert, in front of the tape, printed on record. Eight effects, four macros each. Bypass is a button state.

1. Drip. Short reverb, with decay and drip.
2. Echo. Delay, synced or free, with feedback and tone.
3. Crush. Sample-rate and bit reduction.
4. Tilt. A one-knob tilt EQ, plus drive.
5. Accent. Transient accent, with decay.
6. Chorus. Two voices, with rate and depth.
7. Handset. Band-pass with light distortion.
8. Space. A longer, darker reverb than Drip.

Resample hears the effect. The bay can move the effect macros.

## Sequencers

A sequencer performs the current instrument. The tape holds the arrangement.

This version has two. Steps is a 16-step grid with note, gate, and velocity, for drums. Latch is a latched arpeggio, synced to the internal clock. Swing is on that clock.

## Mixer

Four track faders, pan, and mute, plus master level and a three-band EQ. The encoders follow the selected track: level, pan, low, high. Master is shift-hold.

The master can feed an Icecast server you supply: URL, mount, and password. The default encode is Opus at 96 kbps, with Vorbis as the fallback. The encoder buffer is the latency. The target is under 80 ms. The socket stays off until you arm it. A lit key shows it is open. LIFT does not host the server.

## Patchbay

Sixteen cords. One cord per destination. The last connection wins. Each preset includes a cord set that matches the four macros, so opening the bay does not change the sound until a cord is edited. Disconnect returns that destination to its macro or its default.

Sources: the four macros, the amp envelope, the filter envelope, the LFO, velocity, key track, the voice before the effect, each tape play head, the radio, the input, and the clock (bar phase and step).

Destinations: engine parameters, effect macros, tape speed, reverse, record level, bias, track pan and level, filter cutoff, FM index and feedback, and slice pitch.

Cords run at control rate, 1 kHz, smoothed into the audio block. Three paths may run at audio rate: the radio into an FM index, a tape head into an FM index, and an oscillator into tape speed. Any other audio-rate cord drops to control rate and marks the jack.

Cords belong to the preset. A tape does not store them.

## Projects

A folder, copied by copying the folder.

```
Project.lift/
  project.json
  tapes/
  samples/
  presets/
  bay/
  radio-user.json
```

`project.json` holds the version, the tempo, the tape index, and the screen mode. Audio is wav. Nothing is encrypted. The version starts at 1. A file from a newer version is refused.

## Formats

Standalone for macOS and Windows. A Linux build uses the same audio stack when that backend needs no separate license.

The processor is the same class a VST3 build would wrap. In that build the radio is compiled out where the host blocks sockets.

The internal rate is 48 kHz. A device at another rate is resampled at the edge.

No account. Files stay in the project folder.

## Status

Published 2026-10-08. Updated 2026-10-08: the name catalog, the project loader, and the Eco tape are in this repository. `lift_tests` measures the four-pass overdub. GraphForge pins five fences: project version, audio thread, shipped names, radio client, and the VST3 build. The window, the sample-pool decoder, and the synth voices are not built yet.

## Legal

Copyright © 2026 Martial Systems LLC. All rights reserved. See `LICENSE`.

LIFT is an original Martial Systems instrument. Teenage Engineering and OP-1 are trademarks of their owner. Martial Systems LLC is not affiliated with or endorsed by Teenage Engineering.
