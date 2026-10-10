# Vendored: jidai-common 1.1.2

| | |
| --- | --- |
| Source repo | https://github.com/martialsystems/jidai-collection |
| Path | `jidai-common/` (the whole folder) |
| Branch | `redesign/jidai` |
| Version | **1.1.2** (`project(JidaiCommon VERSION 1.1.2)`) |
| Commit | **8a4b5ae** (8a4b5aecf3d6c2ac8900cc2e88db8feecf8440a8), "jidai-common 1.1.2: AMT 0 transparent, group delay API, lin55 floor (re-vendor this commit)" |
| `jidai-common` tree | 2464d9767032da0eab742650caf8d426df8c1420 |
| Copied with | `git -C <jidai-collection clone> archive 8a4b5ae jidai-common \| tar -x -C third_party` |

The folder was replaced whole from that commit; this file is the only SHOGUN-side addition. The previous vendoring was
1.1.1 at 24ee621 (and 1.1.0 at 9d6e382 before it). 1.1.2 changes, as used here:

- `TripleShaper`: a stage is a wire for a block when its amount `a` is 0 for the whole block, whatever its symmetry
  (the stage law is `y = x` at `a = 0` for any `b`). Before, `a = 0` with `b != 0` ran ADAA on a straight line, a
  two-sample average: half a sample late and about -6 dB at 10 kHz for three such stages at 48 kHz.
- `ShaperControls::isBypass(vcLive)` ignores symmetry (SYM, matrix SYM, VC > SYM): WAVE 0 with the amount trims at 0
  and no VC > AMT depth on a live source is the bit-exact bypass at any SYM.
- `planBlock(ctl, steady, vcLive)`: `steady` covers only the amount inputs; only VC > AMT depth on a live VC keeps a
  zero-amount stage running.
- `runningStages()` and `groupDelay()` report the ADAA's half sample per running stage (not host latency; SHOGUN does
  not report it).
- `pitch::lin55ToVoct(V) = log2(max(V, 1e-3)) - 1.25`: old HZ/V cables are floored at 1 mV (0 V and below give
  -11.216 V, was -5 V), so the lowest values no longer run unbounded below 1 mV.

**Do not edit these files.** Fix them upstream in jidai-collection and re-vendor. What SHOGUN still keeps on its side
is listed in `TESTPLAN.md` under "jidai-common" (the alias fan-out and legacy-name shim in `engine/ports.h`).

Include path: `third_party/jidai-common/include`. That path is set in the Makefile (tests, asan, web) and in
`plugin/CMakeLists.txt`. The engine includes single headers (`jidai/jcs/Detect.h`, `Pitch.h`, `Roles.h`, `JackId.h`,
`jidai/dsp/TripleShaper.h`, `jidai/dsp/Halfband.h`). The freestanding WebAssembly build has small `<string>`,
`<string_view>`, `<optional>` and `<vector>` shims for `JackId.h` (load time only). The plugin and the native tests
include the umbrella `<jidai/CableStandard.h>`.
