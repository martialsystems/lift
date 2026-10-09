# Shift layer (JUCE panel)

SHIFT sits in the top-right keypad slot, above STOP (swapped with DROP).
The keypad top row is now LIFT, LOOP, DROP, REV, SHIFT. The SHIFT key shows
its label only (no icon).

## Engaging shift

- Tap SHIFT (release within 0.3 s): latch on or off.
- Press and hold SHIFT: momentary. Shift ends when you let go.
- Computer keyboard: hold either Shift key for momentary shift. Tab keeps its
  existing job (moving knob focus). Shift already meant "fine" for knob drags,
  the wheel and the arrow keys, so holding it for the layer agrees with that.

While shift is active, the SHIFT pad glows warm red-orange from underneath
(about 120 ms fade in and out), the screen shows the legend, and knobs turn
fine (drag, wheel and arrow keys).

## Map

"Real" means it drives the tape engine or the placeholder voice now.
"Placeholder" means it only sets the model (`LiftProcessor` atomics) for
engines that are not built yet. TAPE, MIX, IN and BAY share the transport map.

| Control | Transport (TAPE/MIX/IN/BAY) | SYNTH | DRUM |
|---|---|---|---|
| White 1-3 | loop in, loop out, loop off (real) | octave -3..-1 (real) | kit 1-3 (placeholder) |
| White 4-7 | arm track 1-4 (real) | octave 0..+3 (real) | kit 4-7 (placeholder) |
| White 8-11 | jump to marker 1-4 (real) | engines LOOM, BEND, FOLD, RATIO (placeholder) | kit 8, length 4/8/12 (placeholder) |
| White 12-14 | to start, to loop in, loop x2 (real) | engines WIRE, SWARM, SPOOL (placeholder) | length 16/24/32 (placeholder) |
| Black 1-3 | speed 0.5x / 1x / 2x (real) | transpose -2/-1/0 (real) | step 1/4, 1/8, 1/16 (placeholder) |
| Black 4-5 | reverse, undo last drop (real) | transpose +1/+2 (real) | step 1/32, 1/8T (placeholder) |
| Black 6-7 | clear armed track (press twice), set marker (real) | step 1/4, 1/8 (placeholder) | swing 0/15 % (placeholder) |
| Black 8-10 | tape character, back 1 s, forward 1 s (real) | step 1/16, 1/32, 1/8T (placeholder) | swing 30/45/60 % (placeholder) |

| Keypad combo | Function |
|---|---|
| SHIFT + REC | record source: SYNTH (real), INPUT (placeholder, records silence), RESAMPLE (real, the tape output) |
| SHIFT + LOOP | loop end at the playhead, loop on (real) |
| SHIFT + STOP | hard stop, no tape-stop ramp (real) |
| SHIFT + LIFT | lift the sum of all four tracks (real) |
| SHIFT + DROP | save to a slot (screen slot picker, see MIDI.md) |
| SHIFT + PLAY | load a slot |
| SHIFT + REV | MIDI learn on/off |

Loop points can now be set anywhere: the old fixed 0-8 s loop is only the
starting value.
