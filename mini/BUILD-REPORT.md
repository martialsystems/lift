# LIFT Mini build report

Branch `mini-pi` (base `tape-engine-juce`), no PR. Written for an outside code
review. Commits covered:

| Commit | What |
|---|---|
| 1f20578 | LIFT Mini: separate Pi standalone, controls, kiosk, install script, arm64 CI |
| e314162 | Review fixes (items 0–6) + RADIO source |
| dcb4675 | CI: build `lift_mini_check` on macOS |
| ddc4fac | COMPUTER AUDIO source (system loopback) + fake-backend tests |
| (this) | This report, screenshots, logs, README fence fix |

## 1. Architecture

LIFT Mini is a second standalone app (`LIFT_Mini`, binary `lift-mini`) built
from the same engine and `LiftProcessor` as full LIFT. Only the UI, the
controls and the app shell are new. It is not a plugin.

```
 Pico 2 panel (USB MIDI) ─┐
 GPIO (optional, chardev) ┼─> MiniControls ─> ShiftState ─> MiniUi ─> LiftProcessor::send()
 keyboard (dev)           ┘                                              │ lock-free fifo_ (1024)
                                                                         v
 ALSA / JACK device ─> AudioDeviceManager ─> LiftProcessor::processBlock (audio thread)
                                                ├─ tape engine (4 tracks x 60 s)
                                                ├─ sliced copy jobs (LIFT / DROP / UNDO)
                                                ├─ RADIO ring read (InternetRadioSource)
                                                └─ COMPUTER ring read (SystemAudioSource)
```

### File map

| Path | Role |
|---|---|
| `mini/MiniApp.cpp` | App shell: window/kiosk, CLI flags (`--fps`, `--device`, `--audio`, `--screenshot F --view radio`) |
| `mini/MiniUi.h/.cpp` | 480x272 (or 800x480) screen: modes, source list, Radio page, status bar |
| `mini/MiniLayout.h` | Layout table (`kScreen480`, `kScreen800`; `-DLIFT_MINI_SCREEN=800`) |
| `mini/MiniControls.h/.cpp` | USB-MIDI panel map, GPIO via `/dev/gpiochipN` v2 ioctls, keyboard |
| `mini/ShiftState.h` | SHIFT state machine (pure logic) and `bipolarKnobToSpeed` |
| `mini/README-pi.md` | Install, controls, audio setup |
| `mini/pi/install.sh`, `lift-mini.service`, `controls.example.json` | Pi install, systemd kiosk, GPIO sample |
| `panel/LiftProcessor.h/.cpp` | Shared processor: command queue, sliced jobs, capture ring, sources |
| `panel/radio/RadioSource.h/.cpp` | RADIO: internet stream decoder thread + ring |
| `panel/sysaudio/SystemAudioSource.h/.cpp` | COMPUTER: capture thread, resample, level, ring |
| `panel/sysaudio/SystemCapturePlatform.cpp` | Linux Pulse monitor + Windows WASAPI loopback backends |
| `panel/sysaudio/SystemCaptureMac.mm` | macOS Core Audio process tap backend |
| `cmake/LiftMini.cmake`, `cmake/LiftApp.cmake` | Targets, plist merge, frameworks |
| `tests/mini_check.cpp`, `tests/mini_bench.h` | Mini checks, callback bench |
| `lift_laws/fences.py`, `tests/test_fences.py` | Name fence (Mini names approved) |
| `.github/workflows/lift-app.yml` | CI: macOS universal, Windows x64, Linux arm64 Mini |
| `mini/docs/` | Screenshots and raw logs (this commit) |

## 2. Pi BOM assumptions (parts only)

From the user's parts sheet:

- Raspberry Pi 5, 4 GB, 64-bit Raspberry Pi OS (Pi 4 also supported; `--fps 20` if busy).
- Waveshare WM8960 Audio HAT (`dtoverlay=wm8960-soundcard`). No line input on it, so line in = a class-compliant USB interface.
- Waveshare 4.3 in DSI screen, 800x480 panel; the UI defaults to the 480x272 layout table, 800 table available.
- Pico 2 panel controller over USB MIDI: scans keys, buttons and 4 encoders (ch 16, CC 20–23 relative, CC 24–27 push).
- 1 GB RAM target for the production Mini: tested only by advice (`total_mem=1024`), not measured. Tape is 60 s per track.

## 3. Callback timing

Method (`tests/mini_bench.h`): 48 kHz, 256-sample blocks (budget 5.33 ms; pass
line is half of it). Fill all 4 tracks with noise, loop the whole 60 s tape,
play, then time every callback for 40 blocks after each command. Report the
worst. Release build. x86_64 numbers are from the 8-core build host; arm64 from
the GitHub `ubuntu-24.04-arm` runner (not a Pi).

| Action | Before (x86_64) | After (x86_64) | After (arm64 CI, run 38681451413) |
|---|---|---|---|
| idle (playing) | – | 0.196 ms | 0.208 ms |
| LIFT (60 s loop) | 3.9 ms | 0.210 ms | 0.224 ms |
| DROP (60 s clip) | 21.0 ms | 0.262 ms | 0.251 ms |
| UNDO DROP | – | 0.220 ms | 0.232 ms |
| SHIFT+LIFT (4 tracks) | 8.6 ms | 0.229 ms | 0.230 ms |
| DROP clip to loop (30 s) | – | 0.251 ms | 0.266 ms |
| UNDO (clip drop) | – | 0.231 ms | 0.252 ms |
| LIFT x3 + DROP x3 burst | 22.8 ms | 0.227 ms | 0.239 ms |

"–" = not recorded before the fix. Raw output: `docs/logs/bench-x86_64-box.log`,
`docs/logs/ci-38681451413-linux-arm64-mini.log`. Not yet measured on a real Pi.

## 4. Review items and resolutions

| # | Item | Resolution | Deviation from the ask |
|---|---|---|---|
| 0 | Long copies in the audio callback (LIFT/DROP/UNDO up to 21 ms) | Copy jobs run **sliced** in the callback: `kJobFrames` = 8192 frames per callback. Commands arriving mid-job wait in `deferred_[64]` in order. | **Sliced copies instead of a worker thread.** Keeps all tape writes on one thread (no locks, no torn tape), at the cost of a job spanning several callbacks (60 s stereo ≈ 350 callbacks ≈ 1.9 s). UNDO restores in slices too, so tests step 0.5 s. |
| 1 | Capture ring could be read while written | `CaptureRing` keeps a 2 s guard behind the write head and zeroes torn frames. | – |
| 2 | Command queue could drop | `fifo_` 1024 + message-thread `backlog_` retried each tick; `queueOverflows` counter for tests. | **Queue backlog** rather than a bigger/blocking queue: nothing is dropped and the audio thread never waits. |
| 3 | SHIFT behaviour ad hoc | `mini/ShiftState.h` state machine (section 5). | – |
| 4 | Speed knob could not go through zero / reverse glitch | Signed speed (section 6); reverse crossfade fix in `read_looped`. | – |
| 5 | Mini names not fenced | `MINI` list in `lift_laws/fences.py`; fence scans `mini/` string literals. | – |
| 6 | Hard-coded layout numbers | `mini/MiniLayout.h` table, 480 and 800 variants. Approved layout unchanged. | – |
| – | Buffer length | Tape kept at **60 s per track** (`kTapeSeconds`) for the 1 GB target. | Shorter than the desktop engine default. |

## 5. SHIFT state machine (`mini/ShiftState.h`)

Times: `kTapMs` 300, `kDoubleMs` 350, `kGraceMs` 150.

| Input | Result |
|---|---|
| Press, release < 300 ms, nothing else touched | Tap = cancel/back |
| Second tap within 350 ms | Latch (SHIFT LOCK). The first tap's cancel already ran (harmless). |
| Tap while latched | Unlatch, no cancel |
| Mode key while latched | Mode opens un-shifted, latch clears |
| SHIFT + any key/button/turn | Combo; release is not a cancel |
| Long press with nothing else | Nothing |
| Release mid-turn | That encoder stays shifted 150 ms after its last detent; others un-shift at once |

## 6. Speed through zero

`bipolarKnobToSpeed` maps the knob to signed speed; the transport uses
`transport_bipolar_speed`. Inside `kStopBand` (|speed| < 0.01) the tape stops
(no denormal crawl). Direction flips only once |speed| passes `kFlipSpeed`
(0.02), so jitter at zero does not chatter. Reverse playback reads with the
corrected loop crossfade in `read_looped`. Test measures the speed before the
block it renders.

## 7. Sources and threading

Both sources follow one pattern: a background `juce::Thread` produces audio,
resamples to 48 kHz, levels it, writes a lock-free `AbstractFifo` ring; the
audio thread only calls a `noexcept` `read()` that never blocks or allocates
and fills silence (with a short fade) when the ring is short.

### RADIO (`panel/radio/`)
- `InternetRadioSource`: JUCE `URL` stream (libcurl on Linux), MP3 via JUCE's
  `MP3AudioFormat`, wrapped in `LiveStream` (length 1<<30, full reads) because
  the stock buffered stream was rejected by the reader.
- 4 s ring, 0.5 s preroll. Network loss → silence + status, reconnect.
- Stations from `radio.json`. Standalone only (`radioAllowed()`).
- Mixed into the output before capture (so LIFT can keep it), or into the IN
  path when REC source = IN.

### COMPUTER (`panel/sysaudio/`)
- `SystemCaptureBackend` interface: `open(why)`, `sampleRate`, `read` (blocks ≤ 50 ms, < 0 = device lost), `close`, `includesOwnOutput`, `name`.
- Ring 2 s, preroll 50 ms, slow-RMS leveling ±12 dB. Turning on flushes stale audio. Full ring drops new audio; the capture thread never waits.
- Open retried every 3 s with the reason shown. 8 s of silence → "SILENT (NOTHING PLAYING, OR NO AUDIO RECORDING PERMISSION)".
- Never sent to the output. If the backend hears LIFT itself (Windows, Linux), it **replaces** LIFT's output in the listen buffer and skips the IN path (no doubling/feedback); if not (macOS tap excludes LIFT), it is **added**.
- macOS 14.2+: `CATapDescription` global stereo tap excluding LIFT's process, private aggregate device, IO block → staging fifo. Older macOS / old SDK → message. `NSAudioCaptureUsageDescription` merged into LIFT and Mini plists.
- Windows: WASAPI shared-mode loopback on default render device (float/16/32-bit).
- Linux/Pi: `dlopen("libpulse-simple.so.0")`, `@DEFAULT_MONITOR@`, float32 48 k stereo, 20 ms fragments. Missing library → unsupported.
- Standalone only; `LIFT_RADIO_OFF=1` disables it (tests).
- UI: full LIFT IN pad 5 (was empty); Mini source list INTERNAL, RADIO, COMPUTER, MASTER.

## 8. Dependency licensing

| Dependency | License | Use |
|---|---|---|
| JUCE 8 | JUCE free plan (commercial terms; splash/revenue cap per JUCE licence) | Framework, MP3 decoder |
| libcurl (system, Linux) | curl licence (MIT-style) | Radio HTTP |
| libpulse-simple (system, runtime `dlopen`) | LGPL-2.1+ | Linux loopback; dynamic, not linked or shipped |
| Core Audio / AudioToolbox / Foundation | Apple SDK | macOS tap |
| WASAPI / COM (ole32) | Windows SDK | Windows loopback |
| Linux GPIO chardev | kernel UAPI header | GPIO (no wiringPi/libgpiod) |

No GPL, copyleft-linked or non-commercial code. No Teenage Engineering or
Origami code; 808/909 reference samples are not in the tree or shipped.
Radio station URLs/stream rights: user's call (open question).

## 9. CI

Workflow `.github/workflows/lift-app.yml`.

| Run | Commit | macOS universal | Windows x64 | Linux arm64 Mini |
|---|---|---|---|---|
| 38676790586 | 1f20578 | success | success | success |
| 38678581210 | e314162 | **failure** (`lift_mini_check` Not Run: not built) | success | success |
| 38679576046 | dcb4675 | success | success | success |
| 38681451413 | ddc4fac | success, 4/4 ctest | success (build only, no tests run) | success, mini checks + bench + headless frame |

Logs: `docs/logs/ci-38681451413-*.log`, `docs/logs/ctest-x86_64-box.log`.

## 10. Untested on hardware / known limitations

- Nothing has run on a real Raspberry Pi: build time, CPU, callback timing, DSI screen, WM8960 HAT, kiosk service.
- Pico 2 panel firmware does not exist yet; MIDI map tested with synthetic MIDI only.
- GPIO path not tested on real lines/encoders.
- 1 GB RAM target not measured.
- COMPUTER audio: macOS tap and its permission prompt, WASAPI loopback, Pulse/PipeWire monitor on Pi — all untested on real machines; only the fake backend runs in tests. Denied macOS permission shows the SILENT message, not a specific one.
- COMPUTER on Linux/Windows hears LIFT itself; replace mode avoids doubling but LIFT's own sound is in the kept audio there.
- Windows CI builds but runs no tests.
- arm64 CI runner is not Pi hardware.
- RADIO: MP3 only (AAC not done); tested live against one public stream from the build host.
- Sliced copies: a 60 s job takes ~1.9 s of callbacks; commands beyond 64 deferred wait in the backlog.
- Screenshots are from a virtual X display, not the DSI panel.

## 11. Open questions

1. Default radio station URLs: which (if any) ship, and their licensing.
2. Should a denied macOS capture permission get its own message (requires probing TCC)?
3. Is 60 s per track right for the 1 GB Mini, or should it scale with RAM?
4. Should Windows CI run the test suite?
5. AAC radio support: needed for v1?
6. Pico 2 firmware: who writes it, and is the MIDI map in README-pi.md final?
7. 480x272 vs 800x480 as the default for the 4.3 in DSI screen.

## 12. Screenshots

- `docs/mini-pi-ui.png` — main screen.
- `docs/mini-pi-radio.png` — Radio page.
