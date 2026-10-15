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
