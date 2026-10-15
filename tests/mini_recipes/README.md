# LIFT Mini scenario book

`lift_mini_session` plays LIFT Mini the way a person does and checks what
happens. Every control goes through the same code path as the app:

```
book.txt step ──> mini::Event ──> MiniUi::handle() ──> LiftProcessor::send()
                  (Pico MIDI goes through mini::midiPanelEvent() first, like MiniApp)
audio: LiftProcessor::processBlock(), 256 samples at a time, offline
time:  a fake clock (ms). It moves only with wait, run and taps.
```

Nothing needs a screen, a sound card, a network or a Pico. It runs anywhere
the Mini builds (Linux x86_64 / arm64, macOS).

## Run it

```sh
cmake -S . -B build -DLIFT_APP_ONLY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target lift_mini_session
build/lift_mini_session_artefacts/Release/lift_mini_session                 # the whole book
build/lift_mini_session_artefacts/Release/lift_mini_session --only "NAME"   # one recipe
build/lift_mini_session_artefacts/Release/lift_mini_session --list          # names
build/lift_mini_session_artefacts/Release/lift_mini_session --chaos-seeds 200   # extra random runs
ctest --test-dir build -R lift_mini_session
```

Screenshots from `shot` go to `./mini-recipes-out/` (`--out DIR` to change).

Output: one line per recipe.

| Word | Meaning | Exit code |
|---|---|---|
| `PASS` | every check held | 0 |
| `FAIL` | a check failed; the line number and the measured value follow | 1 |
| `KNOWN` | a recipe with a `known:` line failed, as expected (a bug not fixed yet) | 0 |
| `FIXED` | a `known:` recipe now passes: delete its `known:` line | 1 |

## Rules for editing (people and AI)

1. **A change to Mini behaviour comes with a recipe.** Add or update the
   recipe in the same commit as the code.
2. **Never loosen a check just to make it pass.** If a number moved, find out
   why. Change the expectation only when the new behaviour is the one you
   want, and say so in the commit message.
3. **A bug you can't fix yet becomes a `known:` recipe**, with one line
   saying what's wrong. Don't delete failing recipes.
4. **When a `known:` recipe reports `FIXED`, delete the `known:` line.** The
   run fails until you do, so fixes can't go unnoticed.
5. **One behaviour per recipe**, with a name that says what should happen
   ("Keys play the synth, on the octave shown"), not what the test does.
6. **Start from scratch every time.** Each recipe begins on a fresh Mini; no
   recipe depends on another.
7. Keep the whole book under about two minutes so it runs on every change.

## File format

```
recipe: Name of the behaviour
  known: why it fails today        # optional; only for bugs not fixed yet
  step ...
  expect ...
```

Lines starting with `#` are comments; a `#` after a step ends the line.
Indentation is only for reading.

### Starting state

A new project: SYNTH screen, page 1, LOOM, 100 BPM, no loop, track 1, octave 0,
speed 1x, stopped, empty tape (60 s per track), no clip, source INTERNAL,
SHIFT off. Each recipe has its own empty slot folder. Drum patterns are cleared
and slots save to a temporary folder.

### Steps

| Step | Does |
|---|---|
| `button NAME [down\|up\|tap]` | A panel button. NAME: `SYNTH DRUM TAPE MIX OCT- OCT+ LIFT DROP REC PLAY SHIFT`. Default `tap` (down, 40 ms, up, 40 ms). |
| `key SPEC [down\|up\|tap]` | A key. SPEC: `1`–`24` chromatic from C3, `w1`–`w14` white keys, `b1`–`b10` black keys, as the hardware sheet counts them. `tap` holds 150 ms. |
| `turn COLOUR N [each [MS]]` | Encoder turn of N detents (negative = left) as one event, like a fast Pico turn. `each` sends N single detents 10 ms apart, like a quick hand (acceleration applies); `each 200` spaces them 200 ms, a slow hand (1 % a detent). COLOUR: `yellow blue black red` (or `1`–`4`). |
| `push COLOUR [down\|up\|tap]` | Encoder press. |
| `midi CH noteon N [VEL]` / `noteoff N` / `cc N V` | A MIDI message as the Pico (or a keyboard) sends it. Channel 16 is the panel map in `mini/MiniControls.h`; anything else goes to the engine. |
| `wait MS` / `run SECONDS` | Let time pass; audio runs. |
| `restart` | Quit (the project saves) and power on again (reopens it). |
| `mark` | Start a new measurement: `out ...`, `moved` and `block_ms` count from here. |
| `input silence \| sine HZ AMP \| noise AMP` | What the audio input hears. |
| `fill track N sine HZ AMP` | Write a tone straight onto a tape track (setup, not playing). |
| `snapshot NAME` | Remember all four tracks (for `changed NAME`). |
| `shot NAME` | Save the screen as `NAME.png`. |
| `paint` | Draw the screen without saving (catches crashes in drawing). |
| `chaos SEED MOVES` | Random presses, keys, turns, SHIFT taps and Pico messages, with random gaps. Checks the invariants after every move, then lets go of everything held. |

### Checks

`expect METRIC OP VALUE`, where OP is `== != < <= > >=`.
`expect toast has TEXT` / `expect toast lacks TEXT` checks the message strip
(case and extra spaces are ignored).

| Metric | Value |
|---|---|
| `mode` | `SYNTH DRUM TAPE MIX RADIO` |
| `page`, `pages` | page on show (1–4), real pages in this mode (SYNTH/DRUM 4, MIX 3, TAPE 1) |
| `page_label has TEXT` | the status bar's mode and n/total (like `toast has`) |
| `octave` | −3…3 |
| `track` | picked track, 1–4 |
| `arm` | the tape's armed track, 1–4 (the processor's view) |
| `source`, `source_sel` | `INTERNAL RADIO COMPUTER MASTER` (chosen / highlighted) |
| `sources_open`, `shift`, `shift_latched` | 0 or 1 |
| `knob COLOUR`, `sknob COLOUR` | that knob's plain / SHIFT parameter on the page on show, 0–1 |
| `tempo`, `engine` | BPM; synth engine (0 = LOOM) |
| `loop` | loop length, seconds (0 = off) |
| `passes` | REC passes started (the processor's pass count) |
| `character` | tape character row (0 DECK … 3 CAP) |
| `fader N`, `pan N` | track N's level (gain²) and pan (−1…1) |
| `mute N` | 0 or 1 |
| `speed` | signed tape speed (−4…4) |
| `playing`, `recording` | 0 or 1 |
| `pos` | playhead, seconds |
| `moved` | playhead travel since `mark`, seconds (negative = backwards) |
| `voices` | synth voices sounding |
| `note` | the synth's newest held note (−1 = none) |
| `drums` | drum voices sounding |
| `clip` | the held clip's length in seconds (0 = none) |
| `clip_peak`, `clip_rms` | the held clip's level (non-finite reads as 1e9) |
| `pool` | clips kept this session |
| `slice_kit` | a DROP slice kit is loaded |
| `out rms` / `out peak` / `out clicks` | output since `mark` (clicks = biggest sample-to-sample jump) |
| `track_rms N [A B]` | a tape track's level, optionally from A to B seconds |
| `changed NAME` | biggest difference from snapshot NAME |
| `job_busy` | a sliced LIFT / DROP / UNDO copy still running |
| `overflows` | commands that waited in the backlog |
| `block_ms` | slowest audio callback since `mark`, ms (budget 5.33) |

Every run also fails if the output goes NaN or infinite.

## Adding to the runner

Everything is in `tests/mini_session.cpp`:

- **A new step:** add a branch in `Session::step()`. Send controls through
  `send()`, `press()`, `keyDown()` or `midi()`, never by calling the
  processor directly, so the test follows the real path.
- **A new metric:** add a line in `Session::value()`, reading from
  `ui_->view()` (UI state) or the processor's public atomics (`uiPlaying`,
  `uiVoiceEnv`, …), then a row in the table above.
- **New UI state to check:** add a field to `MiniUi::View` and fill it in
  `MiniUi::view()` (`mini/MiniUi.cpp`). This is the only test hook in the
  Mini code, besides `setClock()`.
- **A new control or mode:** add its name to `kModeNames`, or extend
  `Btn` and `btnFromName()` in `mini/MiniControls.*`; `button NAME` then
  works with no runner change.

After any runner change, run the whole book and `--chaos-seeds 50` before
committing.

The `block_ms` check (the audio callback budget) only means something in a
Release build. A Debug or sanitizer build is several times slower and fails it;
use those builds to hunt memory errors, not to time things.
