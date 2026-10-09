# Copyright (c) 2026 Martial Systems LLC. All rights reserved.
"""Block path uses a false fact. Allow path measures this tree."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(Path.home() / "graphforge" / "src")]

from graphforge import LawBlockedError, require_law
from graphforge import __version__ as graphforge_version

from lift_laws.fences import (
    APPROVED,
    audio_decision,
    audio_text_problems,
    build_gate,
    draft_words,
    measure_all,
    names_decision,
    radio_decision,
    radio_text_problems,
    version_decision,
    vst3_decision,
    vst3_entry_problems,
    vst3_region_problems,
)
from product_laws import laws

COPYRIGHT = "Copyright (c) 2026 Martial Systems LLC. All rights reserved."


def expect(cond: bool, message: str, failures: list[str]) -> None:
    if not cond:
        failures.append(message)


def copyright_gaps() -> list[str]:
    gaps: list[str] = []
    paths = [ROOT / "CMakeLists.txt", ROOT / "product_laws.py"]
    for folder in ("src", "tests", "tools", "lift_laws"):
        paths.extend(path for path in (ROOT / folder).rglob("*") if path.suffix in {".py", ".h", ".cpp"})
    for path in paths:
        first = path.read_text(encoding="utf-8").splitlines()[0]
        if COPYRIGHT not in first:
            gaps.append(str(path.relative_to(ROOT)))
    return gaps


def main() -> int:
    failures: list[str] = []
    expect(version_decision(False) == "block", "version block", failures)
    expect(audio_decision(False) == "block", "audio block", failures)
    expect(names_decision(False) == "block", "names block", failures)
    expect(radio_decision(False) == "block", "radio block", failures)
    expect(vst3_decision(False) == "block", "vst3 block", failures)
    expect(version_decision(True) == "allow", "version allow", failures)

    malloc_src = "void f() { void* p = malloc(4); }\n"
    expect(audio_text_problems(malloc_src) != [], "malloc not seen", failures)
    expect(audio_text_problems("void f() { prepare_tracks(rt, 8); }\n") != [], "prepare_tracks", failures)
    expect(audio_text_problems("void f() { project_write(dir, info, rt); }\n") != [], "project_write", failures)
    comment = "// Audio thread. No allocation, no disk, no socket.\nvoid f() {}\n"
    expect(audio_text_problems(comment) == [], f"comment false fail {audio_text_problems(comment)}", failures)
    block = "/* socket and fopen stay in this note */\nvoid f() {}\n"
    expect(audio_text_problems(block) == [], "block comment false fail", failures)

    expect(draft_words("Phase-distortion") == ["Phase"], "hyphen boundary", failures)
    expect(draft_words("phase-distortion") == [], "lowercase role", failures)
    expect("Phase" not in APPROVED, "draft listed as approved", failures)

    expect(radio_text_problems("void relay();") != [], "relay", failures)
    expect(radio_text_problems("int lift_radio_tune();") == [], "tune is a client", failures)

    bad_region = "add_library(lift_vst3_core STATIC x.cpp)\ntarget_link_libraries(lift_vst3_core PUBLIC lift_radio)\n"
    expect(vst3_region_problems(bad_region) != [], "vst3 link", failures)
    good_region = (
        "add_library(lift_vst3_core STATIC src/plugin/vst3_entry.cpp)\n"
        "target_link_libraries(lift_vst3_core PUBLIC lift_core)\n"
        "target_compile_definitions(lift_vst3_core PUBLIC LIFT_RADIO=0)\n"
    )
    expect(vst3_region_problems(good_region) == [], f"clean region {vst3_region_problems(good_region)}", failures)
    guard = '#if defined(LIFT_RADIO) && LIFT_RADIO\n#error "VST3 build has no radio"\n#endif\n'
    expect(vst3_entry_problems(guard) == [], f"error text {vst3_entry_problems(guard)}", failures)
    included = guard + '#include "radio/client.h"\n'
    expect(vst3_entry_problems(included) != [], "radio include", failures)

    try:
        require_law(
            build_gate("lift.negative")(),
            {"ok": False},
            allow_decisions={"allow"},
            audit=False,
            law_id="lift.negative",
        )
        failures.append("negative require_law allowed")
    except LawBlockedError:
        pass

    pin = tuple(int(part) for part in "0.6.0".split("."))
    have = tuple(int(part) for part in graphforge_version.split(".")[:3])
    expect(have >= pin, f"graphforge {graphforge_version}", failures)

    measured = measure_all(ROOT)
    for key, problems in measured.items():
        if problems:
            failures.append(f"{key}: {problems}")
    if not failures:
        for spec in laws():
            if spec["state"]["ok"] is not True:
                failures.append(f"{spec['id']} not measured clean")
                continue
            try:
                require_law(
                    spec["build"](),
                    spec["state"],
                    allow_decisions=spec["allow_decisions"],
                    audit=False,
                    law_id=spec["id"],
                )
            except LawBlockedError as exc:
                failures.append(f"{spec['id']} blocked: {exc}")
    gaps = copyright_gaps()
    if gaps:
        failures.append(f"copyright: {gaps}")

    if failures:
        for item in failures:
            print(item)
        return 1
    print("ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
