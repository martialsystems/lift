# LIFT Mini on a Raspberry Pi

`LIFT_Mini` (binary `lift-mini`) is a separate standalone for the Mini. It runs the same LIFT engine as the desktop app, but the window is the Mini's 4.3 in screen: drawn at 480 × 272 and scaled to fit (800 × 480 on the Waveshare DSI panel). It uses plain 2D drawing with no OpenGL, and the frame rate is capped (30 fps by default).

Target: the prototype in the parts sheet. That's a Raspberry Pi 5 (4 GB) on 64-bit Raspberry Pi OS, a Waveshare WM8960 Audio HAT, a Waveshare 4.3 in DSI screen and a Pico 2 panel controller over USB. A Pi 4 works too.

## Install (one command)

```bash
git clone -b mini-pi https://github.com/martialsystems/lift.git && cd lift && ./mini/pi/install.sh
```

The script does the following:

- Installs the apt build dependencies.
- Builds `LIFT_Mini` in Release. The first build takes about 15–25 minutes on a Pi 5. Boards with less than 3 GB of RAM build with 2 jobs.
- Installs the binary to `/usr/local/bin/lift-mini`.
- Adds you to the `audio` and `gpio` groups and gives the audio group real-time priority.
- Turns on desktop autologin.
- Enables a systemd user service (`lift-mini.service`) that starts LIFT Mini full screen at login.

Reboot once afterwards. Use `./mini/pi/install.sh --no-service` to skip the autostart.

- Stop / start: `systemctl --user stop lift-mini` / `systemctl --user start lift-mini`
- Logs: `journalctl --user -u lift-mini -f`

## Options

| Flag | Default | |
|---|---|---|
| `--kiosk` | off | Full screen, no title bar, screen blanking off |
| `--fps N` | 30 | UI frame cap (5–60). Use 20 on a Pi 4 if the CPU is busy |
| `--size WxH` | 960x544 | Window size when not in kiosk mode |
| `--audio ALSA\|JACK` | ALSA | Audio backend |
| `--device NAME` | system default | ALSA/JACK device name (e.g. the WM8960 card) |
| `--buffer N` | 256 | Buffer in samples. The engine runs at 48 kHz, so 256 = 5.3 ms |
| `--gpio FILE` | `/etc/lift-mini/controls.json` | GPIO map. If the file is missing, GPIO is off |
| `--screenshot FILE` | | Render one frame to PNG and quit (headless checks) |

## Controls

All three inputs feed the same handler, so they can be mixed.

**1. Panel over USB MIDI (the parts-sheet plan: a Pico 2 scans the keys, buttons and encoders).** The Pico firmware should send:

| What | MIDI |
|---|---|
| 24 keys | Channel 1, notes 48–71 (these go to the engine as normal notes) |
| 10 buttons | Channel 16, note on/off. 0 SYNTH, 1 DRUM, 2 TAPE, 3 MIX, 4 OCT−, 5 OCT+, 6 LIFT, 7 DROP, 8 REC, 9 PLAY/STOP, 10 SHIFT |
| 4 encoders turn | Channel 16, CC 20–23 (yellow, blue, black, red), relative: 65 = +1, 63 = −1 |
| 4 encoder push | Channel 16, CC 24–27, 127 down / 0 up |

Any other MIDI keyboard or controller also works: notes, clock and CCs as in `panel/MIDI.md`.

**2. GPIO wired straight to the Pi (optional).** Copy `/etc/lift-mini/controls.json.example` to `controls.json` and set the BCM line numbers. It covers buttons by name, up to 24 keys, and encoders (A, B, push). Inputs are active-low with the internal pull-ups on. Lines are read through the kernel's GPIO character device (`/dev/gpiochipN`, v2 ioctls), so no GPIO library is needed: no wiringPi, no libgpiod. Lines are polled at 1 kHz, buttons are debounced over 5 ms, and EC11 encoders are decoded at 4 steps per detent. A single request is limited to 64 lines, so a full 24-key matrix belongs on the Pico, not the Pi.

**3. Computer keyboard (desk testing).**

| Key | Action |
|---|---|
| Z S X D C V G B H N J M | Keys 1–12 |
| Q 2 W 3 E R 5 T 6 Y 7 U | Keys 13–24 |
| F1–F4 | SYNTH, DRUM, TAPE, MIX (press again for the next page) |
| − / = | OCT down / up |
| L | LIFT |
| P | DROP |
| O | REC |
| Space | PLAY/STOP |
| Tab | SHIFT (hold) |
| , . | Turn the yellow knob |
| [ ] | Turn the blue knob |
| 9 0 | Turn the black knob |
| ; ' | Turn the red knob |

What the controls do:

- LIFT keeps the last few seconds heard. SHIFT+LIFT keeps the four tracks' sum.
- DROP places the clip by mode: TAPE/MIX onto the selected track, SYNTH onto the keys, DRUM as 24 slices.
- REC overdubs the selected track.
- Knob presses select the track in TAPE and mute it in MIX. In other modes, SHIFT + press resets the knob.
- SHIFT + keys 1–8 saves a slot.
- SHIFT + OCT− undoes the last DROP.

## SHIFT

SHIFT is a small state machine (`mini/ShiftState.h`). It is tested in `tests/mini_check.cpp`.

| You do | What happens |
|---|---|
| Keep SHIFT pressed | SHIFT is on until you let go. |
| Tap SHIFT (under 300 ms, nothing else touched) | Cancel / back. It closes the source list or clears a message. |
| Double-tap (second tap within 350 ms) | SHIFT latches on. The first tap's cancel has already happened, which is harmless. The status bar shows SHIFT LOCK. |
| Tap while latched | Unlatches. No cancel. |
| Mode key while latched | The new mode opens un-shifted (the latch clears). |
| SHIFT + any key, button or turn | It's a combo, so releasing SHIFT afterwards is not a cancel. |
| SHIFT + turn: fine steps (0.5 %) | Releasing SHIFT mid-turn keeps that encoder shifted for 150 ms after its last detent, so a turn that outlasts the finger doesn't jump to the plain parameter. Other encoders go un-shifted at once. |
| Long press alone (over 300 ms) | Nothing. |

## TAPE speed (yellow knob)

- The knob runs from reverse through stop to forward (−4x … 0 … +4x).
- The centre band (±4 % of travel) is a stop: the tape stands still while PLAY is on.
- Near the centre, speed rises linearly up to 0.25x. Past that it is exponential, and it snaps to fifths and octaves (0.25, ⅓, 0.5, ⅔, 1, 1.5, 2, 3, 4x).
- A change of direction always glides the tape down below 0.02x before it flips, so the heads never jump from +x to −x.
- The engine never divides by speed.
- In reverse, the loop crossfade reads the wet side backwards into the loop end. Before this fix it ran forwards against the dry side.

## RADIO (internet radio for now)

- **Opening it:** SHIFT + REC opens the source list (INTERNAL, RADIO, MASTER). Yellow picks and a yellow press opens. RADIO opens the RADIO page.
- **The page's knobs** (sheet 4.5):
  - Yellow: TUNE (the next or previous station).
  - Blue: PRESET 1–8.
  - Black: LEVEL.
  - Red: MONO blend.
- **What it plays into:** the radio goes into the mix, or into the IN path when REC source is IN, before the always-on capture. **LIFT keeps what the radio played**, like any other source.
- **Stations:** `~/.config/Martial Systems/LIFT/radio.json` (written on first use) holds `stations` (name and URL) and `presets` (8 station indexes). Edit it and restart. Only MP3 streams are supported. AAC isn't in this build.
- **How it works:**
  - Streams are decoded on a background thread by JUCE's MP3 reader. There is no ffmpeg or libmad.
  - The audio is resampled to 48 kHz and levelled toward the keep target (a slow RMS, ±12 dB).
  - It is handed to the audio thread through a lock-free 4 s ring, which plays after 0.5 s of preroll.
  - On network loss the audio thread gets silence (it never waits). The page shows `NO SIGNAL (...)`, and the stream retries after 1, 2, 4 … 30 s.
- **Shared code:** `RadioSource` is the interface. The FM tuner (Si47xx) will be a second implementation.
- **Full LIFT:** on the IN screen, picking RADIO plays STATION 01–05 (the first five stations). The status shows as a flash, with no layout change. Radio is in the standalone apps only. The VST3/AU plug-ins have no radio.
- **Station licences:** the default stations (SomaFM, Radio Paradise) are public streams for testing only. Shipping any default station is a licensing decision.

## Screen size

All drawing comes from one layout table (`mini/MiniLayout.h`). The default is the spec's 480 × 272, scaled up. For the native 800 × 480 panel, build with `-DCMAKE_CXX_FLAGS=-DLIFT_MINI_SCREEN=800` (one switch).

## Real-time notes

These live in the shared engine, so full LIFT gets them too:

- **No long copies in one callback.** LIFT, SHIFT+LIFT, DROP, DROP onto a loop and UNDO run as sliced jobs of at most 8,192 frames per callback.
  - Copy commands that arrive while a job is running wait in order.
  - Clips are swapped by pointer and kept alive off the audio thread.
- **The capture ring** is allocated once, with a 2 s guard. LIFT snapshots the write index, then copies. A copy that ever gets lapped has its overwritten head zeroed, never torn.
- **The command queue** holds 1,024 commands. A full queue never drops a press: the command waits in a message-thread backlog, and it is counted (`queueOverflows`) and logged. Knobs are atomics, so they coalesce by themselves.
- **Benchmark:** `lift_mini_check` measures the worst callback (256 frames at 48 kHz, 5.33 ms budget) during each operation, and fails above half the budget.

## Audio

The WM8960 HAT needs its overlay. Per Waveshare's wiki for the Pi 5, add `dtoverlay=wm8960-soundcard` to `/boot/firmware/config.txt` and reboot. Then check that `aplay -l` lists it and pass `--device` if it isn't the default card. The HAT has no line input, so use a class-compliant USB interface for line in (as the parts sheet says). For JACK, start `jackd -d alsa -r 48000 -p 256` first and run with `--audio JACK`.

To test the Mini's 1 GB target (from the parts sheet), set `total_mem=1024` in `config.txt`. The engine keeps 60 s of tape per track.

## Licences

- No GPL code is used.
- JUCE 8 is used under its licence (free plan; splash screen off per JUCE 8 terms).
- ALSA (`libasound`, LGPL) and JACK (`libjack`, LGPL) are linked dynamically.
- GPIO uses only the kernel headers.

## Known limits

- JUCE draws through X11. The default Pi OS desktop (Wayland) runs it through XWayland, which works. On a Lite image, install a minimal X server (`xserver-xorg xinit`) and start it with `startx /usr/local/bin/lift-mini --kiosk`.
- These pieces are on the spec but not in this build yet: the Mini's page contents beyond page 1 of each mode (pages 2–4 show the same view), REDO, JACKS setup, the radio, and the FM tuner.

## COMPUTER audio source

SHIFT+REC → COMPUTER captures what the system is playing (standalone only)
into the listen buffer so it can be LIFTed. It is never sent to LIFT's output.

- Pi / Linux: needs PulseAudio or PipeWire (`pipewire-pulse`) and
  `libpulse-simple.so.0` (`sudo apt install libpulse0`). It reads the default
  output's monitor. Without it the source shows as unsupported.
- macOS 14.2+: Core Audio process tap, LIFT's own output excluded. macOS asks
  for "System Audio Recording" permission the first time.
- Windows: WASAPI loopback on the default output.
- `LIFT_RADIO_OFF=1` also disables computer audio (used by tests).
