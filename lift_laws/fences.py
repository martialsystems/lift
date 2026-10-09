# Copyright (c) 2026 Martial Systems LLC. All rights reserved.
"""Five fail-closed fences. Each one measures this tree, then allows or blocks.

Verify-before-report covers README.md and LICENSE. These fences do not.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

ENGINES = ("Loom", "Bend", "Fold", "Ratio", "Wire", "Swarm", "Spool", "Spare")
EFFECTS = ("Drip", "Echo", "Crush", "Tilt", "Accent", "Chorus", "Handset", "Space")
SEQUENCERS = ("Steps", "Latch")
CHARACTERS = ("Deck", "Pocket", "Shed", "Cap")
DRUMS = ("Tap",)
GLYPH_OWNERS = ENGINES + DRUMS + EFFECTS + SEQUENCERS
APPROVED = ENGINES + EFFECTS + SEQUENCERS + CHARACTERS + DRUMS
DRAFT = (
    "Pulse",
    "Phase",
    "String",
    "Cluster",
    "Sampler",
    "Box",
    "Spring",
    "Punch",
    "Phone",
    "Grid",
    "Hold",
    "Studio",
    "Porta",
    "Vintage",
    "Disc",
)
RADIO_FUNCS = frozenset({"lift_radio_tune", "lift_radio_read", "lift_radio_capture"})

_DRAFT = frozenset(DRAFT)
_WORD = re.compile(r"[A-Za-z]+")
_FORBIDDEN = re.compile(
    r"\b(?:malloc|calloc|realloc|fopen|fclose|socket|connect|recv|send|listen|accept|new|delete"
    r"|prepare_tracks|release_tracks|project_read_info|project_read_audio|project_write"
    r"|lift_radio_tune|lift_radio_read|lift_radio_capture"
    r"|pool_import|pool_add_pcm|pool_load|pool_read|pool_set_map)\b"
    r"|std::"
    r"|#include\s*[<\"](?:fstream|string|vector)[>\"]"
    r"|#include\s*\"(?:project|radio)/"
)
_CAPTURE_BAN = re.compile(r"\b(?:socket|connect|recv|send|listen|accept|fopen)\b")
_C_ARRAY = re.compile(r"const char\* const (\w+)\[\] = \{(.*?)\};", re.S)
_ROWS = re.compile(r"constexpr Character kRows\[kCharacterCount\] = \{(.*?)\};", re.S)
_ROW_NAME = re.compile(r'\{\s*"([^"]+)"')
_RELAY = re.compile(r"\b(?:relay|rebroadcast)\b", re.I)
_RADIO_SYM = re.compile(r"\blift_radio_[A-Za-z0-9_]+")
_GLYPH = re.compile(r"[A-Za-z]+\.[A-Za-z]+")


def gate_decision(ok: bool) -> str:
    return "allow" if ok is True else "block"


def version_decision(passed: bool) -> str:
    return gate_decision(passed)


def audio_decision(clean: bool) -> str:
    return gate_decision(clean)


def names_decision(clean: bool) -> str:
    return gate_decision(clean)


def radio_decision(clean: bool) -> str:
    return gate_decision(clean)


def vst3_decision(clean: bool) -> str:
    return gate_decision(clean)


def strip_cpp_comments(text: str) -> tuple[str, bool]:
    """Return source with comments removed, and whether a block comment stays open."""
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("//", i):
            nl = text.find("\n", i)
            if nl < 0:
                break
            i = nl
            continue
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            if end < 0:
                return "".join(out), True
            i = end + 2
            continue
        if text[i] == '"':
            out.append(text[i])
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == "\\" and i + 1 < n:
                    out.append(text[i + 1])
                    i += 2
                    continue
                if text[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        out.append(text[i])
        i += 1
    return "".join(out), False


def audio_text_problems(text: str) -> list[str]:
    stripped, unclosed = strip_cpp_comments(text)
    problems: list[str] = []
    if unclosed:
        problems.append("unclosed comment")
    seen: set[str] = set()
    for match in _FORBIDDEN.finditer(stripped):
        token = match.group(0)
        if token not in seen:
            seen.add(token)
            problems.append(token)
    return problems


def _scan_pairs(text: str, open_at: int, opener: str, closer: str) -> int | None:
    depth = 0
    i = open_at
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == '"':
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        if ch == opener:
            depth += 1
        elif ch == closer:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return None


def function_body(text: str, name: str) -> str | None:
    stripped, unclosed = strip_cpp_comments(text)
    if unclosed:
        return None
    start = 0
    while True:
        at = stripped.find(name, start)
        if at < 0:
            return None
        before = stripped[at - 1] if at else " "
        if before.isalnum() or before == "_" or not stripped.startswith("(", at + len(name)):
            start = at + len(name)
            continue
        paren = _scan_pairs(stripped, at + len(name), "(", ")")
        if paren is None:
            return None
        i = paren + 1
        while i < len(stripped) and stripped[i] in " \t\r\n":
            i += 1
        if stripped.startswith("noexcept", i):
            i += len("noexcept")
            while i < len(stripped) and stripped[i] in " \t\r\n":
                i += 1
        if i >= len(stripped) or stripped[i] != "{":
            start = at + len(name)
            continue
        end = _scan_pairs(stripped, i, "{", "}")
        if end is None:
            return None
        return stripped[i + 1 : end]


def callback_source_problems(text: str) -> list[str]:
    body = function_body(text, "lift_radio_read")
    if body is None:
        return ["lift_radio_read body missing"]
    body = re.sub(r"\blift_radio_read\b", "", body)
    return audio_text_problems(body)


def capture_source_problems(text: str) -> list[str]:
    body = function_body(text, "lift_radio_capture")
    if body is None:
        return ["lift_radio_capture body missing"]
    found: list[str] = []
    for match in _CAPTURE_BAN.finditer(body):
        token = match.group(0)
        if token not in found:
            found.append(token)
    return found


def draft_words(text: str) -> list[str]:
    found: list[str] = []
    seen: set[str] = set()
    for word in _WORD.findall(text):
        if word in _DRAFT and word not in seen:
            seen.add(word)
            found.append(word)
    return found


def vst3_region(cmake: str) -> str | None:
    start = cmake.find("add_library(lift_vst3_core")
    if start < 0:
        return None
    rest = cmake[start:]
    end = re.search(r"\nadd_executable\b", rest)
    if end:
        rest = rest[: end.start()]
    return rest


def vst3_region_problems(region: str | None) -> list[str]:
    if region is None:
        return ["missing lift_vst3_core"]
    problems: list[str] = []
    if "LIFT_RADIO=0" not in region:
        problems.append("LIFT_RADIO=0 missing")
    if re.search(r"\blift_core\b", region) is None:
        problems.append("lift_core missing")
    if re.search(r"\blift_radio\b", region):
        problems.append("links lift_radio")
    if "src/radio" in region:
        problems.append("src/radio in the VST3 target")
    return problems


def vst3_entry_problems(text: str) -> list[str]:
    problems: list[str] = []
    if "#error" not in text or "LIFT_RADIO" not in text:
        problems.append("missing VST3 radio guard")
    if re.search(r'#include\s*"radio/', text):
        problems.append("includes radio")
    if "lift_radio_" in text:
        problems.append("references lift_radio_")
    return problems


def radio_text_problems(text: str) -> list[str]:
    problems: list[str] = []
    if _RELAY.search(text):
        problems.append("relay")
    return problems


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _manifest_paths(root: Path) -> tuple[list[Path], list[str]]:
    manifest = root / "src" / "audio" / "AUDIO_THREAD.txt"
    if not manifest.is_file():
        return [], ["AUDIO_THREAD.txt missing"]
    paths: list[Path] = []
    problems: list[str] = []
    for raw in _read(manifest).splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        rel = Path(line)
        if rel.is_absolute() or ".." in rel.parts:
            problems.append(f"audio path escapes: {line}")
            continue
        path = root / rel
        if not path.is_file() or not path.resolve().is_relative_to((root / "src").resolve()):
            problems.append(f"audio file missing: {line}")
            continue
        paths.append(path)
    if not paths and not problems:
        problems.append("audio manifest empty")
    return paths, problems


def measure_audio(root: Path) -> list[str]:
    paths, problems = _manifest_paths(root)
    for path in paths:
        for item in audio_text_problems(_read(path)):
            problems.append(f"{path.relative_to(root)}: {item}")
    return problems


def _c_arrays(text: str) -> dict[str, tuple[str, ...]] | None:
    found = {name: tuple(re.findall(r'"([^"]*)"', body)) for name, body in _C_ARRAY.findall(text)}
    needed = ("kEngines", "kEffects", "kSequencers", "kCharacters", "kDrums", "kGlyphs")
    if any(name not in found for name in needed):
        return None
    return found


def _character_names(text: str) -> tuple[str, ...] | None:
    match = _ROWS.search(text)
    if not match:
        return None
    return tuple(_ROW_NAME.findall(match.group(1)))


def _text_files(root: Path) -> list[Path]:
    files = [root / "README.md", root / "LICENSE"]
    for folder in ("presets", "src"):
        base = root / folder
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.is_file() and path.suffix not in {".wav", ".aif", ".aiff", ".flac"}:
                files.append(path)
    return files


def measure_names(root: Path) -> list[str]:
    problems: list[str] = []
    catalog_path = root / "src" / "names" / "catalog.cpp"
    if not catalog_path.is_file():
        return ["catalog.cpp missing"]
    arrays = _c_arrays(_read(catalog_path))
    if arrays is None:
        problems.append("catalog arrays missing")
    else:
        expected = {
            "kEngines": ENGINES,
            "kEffects": EFFECTS,
            "kSequencers": SEQUENCERS,
            "kCharacters": CHARACTERS,
            "kDrums": DRUMS,
        }
        for key, want in expected.items():
            if arrays[key] != want:
                problems.append(f"{key} is {arrays[key]!r}")
        glyphs = arrays["kGlyphs"]
        owners: list[str] = []
        counts: dict[str, int] = {}
        for glyph in glyphs:
            if _GLYPH.fullmatch(glyph) is None:
                problems.append(f"glyph {glyph}")
                continue
            owner = glyph.split(".", 1)[0]
            counts[owner] = counts.get(owner, 0) + 1
            if not owners or owners[-1] != owner:
                owners.append(owner)
        if tuple(owners) != GLYPH_OWNERS:
            problems.append(f"glyph owners {tuple(owners)!r}")
        for owner in GLYPH_OWNERS:
            if counts.get(owner) != 4:
                problems.append(f"{owner} glyphs {counts.get(owner, 0)}")
    rows_path = root / "src" / "tape" / "character.cpp"
    rows = _character_names(_read(rows_path)) if rows_path.is_file() else None
    if rows != CHARACTERS:
        problems.append(f"character rows {rows!r}")
    preset = root / "presets" / "factory" / "index.json"
    try:
        data = json.loads(_read(preset))
    except (OSError, json.JSONDecodeError) as exc:
        problems.append(f"preset index: {exc}")
    else:
        for key, want in (
            ("engines", ENGINES),
            ("effects", EFFECTS),
            ("sequencers", SEQUENCERS),
            ("characters", CHARACTERS),
            ("drums", DRUMS),
        ):
            got = tuple(data.get(key, ()))
            if got != want:
                problems.append(f"preset {key} is {got!r}")
    readme = _read(root / "README.md") if (root / "README.md").is_file() else ""
    present = set(_WORD.findall(readme))
    for name in APPROVED:
        if name not in present:
            problems.append(f"README missing {name}")
    for path in _text_files(root):
        try:
            text = _read(path)
        except UnicodeError:
            problems.append(f"unreadable {path.relative_to(root)}")
            continue
        for word in draft_words(text):
            problems.append(f"{path.relative_to(root)}: {word}")
    return problems


def measure_radio(root: Path) -> list[str]:
    folder = root / "src" / "radio"
    files = sorted(folder.glob("*")) if folder.is_dir() else []
    files = [path for path in files if path.suffix in {".h", ".cpp"}]
    if not files:
        return ["radio client missing"]
    problems: list[str] = []
    found: set[str] = set()
    saw_read = False
    saw_capture = False
    for path in files:
        text = _read(path)
        for item in radio_text_problems(text):
            problems.append(f"{path.name}: {item}")
        found.update(_RADIO_SYM.findall(text))
        if function_body(text, "lift_radio_read") is not None:
            saw_read = True
            for item in callback_source_problems(text):
                problems.append(f"{path.name}: {item}")
        if function_body(text, "lift_radio_capture") is not None:
            saw_capture = True
            for item in capture_source_problems(text):
                problems.append(f"{path.name}: {item}")
    if not saw_read:
        problems.append("lift_radio_read body missing")
    if not saw_capture:
        problems.append("lift_radio_capture body missing")
    if found != RADIO_FUNCS:
        problems.append(f"radio symbols {sorted(found)!r}")
    return problems


def _nm(path: Path) -> tuple[str, str | None]:
    if not path.is_file():
        return "", f"missing {path.name}"
    proc = subprocess.run(["nm", str(path)], capture_output=True, text=True)
    if proc.returncode != 0:
        return "", f"nm {path.name} failed"
    return proc.stdout, None


def measure_vst3(root: Path) -> list[str]:
    cmake_path = root / "CMakeLists.txt"
    entry_path = root / "src" / "plugin" / "vst3_entry.cpp"
    problems: list[str] = []
    if not cmake_path.is_file():
        problems.append("CMakeLists.txt missing")
    else:
        problems.extend(vst3_region_problems(vst3_region(_read(cmake_path))))
    if not entry_path.is_file():
        problems.append("vst3_entry.cpp missing")
    else:
        problems.extend(vst3_entry_problems(_read(entry_path)))
    blobs = {
        "vst3": root / "build" / "liblift_vst3_core.a",
        "core": root / "build" / "liblift_core.a",
        "radio": root / "build" / "liblift_radio.a",
    }
    text: dict[str, str] = {}
    for key, path in blobs.items():
        blob, error = _nm(path)
        if error:
            problems.append(error)
        else:
            text[key] = blob
    if "lift_radio" in text.get("vst3", ""):
        problems.append("vst3 archive contains lift_radio")
    if "lift_radio" in text.get("core", ""):
        problems.append("core archive contains lift_radio")
    if "radio" in text and "lift_radio_tune" not in text["radio"]:
        problems.append("radio archive missing lift_radio_tune")
    return problems


def measure_version(root: Path) -> list[str]:
    exe = root / "build" / "lift_tests"
    if not exe.is_file():
        return ["lift_tests missing"]
    proc = subprocess.run(
        [str(exe), "--only", "version"],
        cwd=root,
        capture_output=True,
        text=True,
        timeout=60,
    )
    if proc.returncode != 0:
        detail = (proc.stderr or proc.stdout).strip()
        return [f"version test exit {proc.returncode}: {detail}"]
    if not proc.stdout.strip().endswith("ok"):
        return ["version test did not print ok"]
    return []


def ensure_build(root: Path) -> None:
    needed = (
        root / "build" / "lift_tests",
        root / "build" / "liblift_vst3_core.a",
        root / "build" / "liblift_radio.a",
        root / "build" / "liblift_core.a",
    )
    if os.environ.get("LIFT_NO_REBUILD") == "1":
        missing = [path.name for path in needed if not path.is_file()]
        if missing:
            raise SystemExit(f"LIFT_NO_REBUILD missing {missing}")
        return
    subprocess.run(
        ["cmake", "-S", str(root), "-B", str(root / "build"), "-DCMAKE_BUILD_TYPE=Release"],
        cwd=root,
        check=True,
    )
    subprocess.run(
        [
            "cmake",
            "--build",
            str(root / "build"),
            "--target",
            "lift_tests",
            "lift_vst3_core",
            "lift_radio",
            "lift_core",
        ],
        cwd=root,
        check=True,
    )


def measure_all(root: Path | None = None) -> dict[str, list[str]]:
    root = root or ROOT
    ensure_build(root)
    return {
        "version": measure_version(root),
        "audio": measure_audio(root),
        "names": measure_names(root),
        "radio": measure_radio(root),
        "vst3": measure_vst3(root),
    }


def build_gate(law_id: str):
    def build():
        from graphforge import END, START, StateGraph, last_value
        from graphforge.state import ChannelSpec, StateSchema

        schema = StateSchema.from_specs(
            [
                ChannelSpec("ok", last_value, default=False),
                ChannelSpec("decision", last_value, default=None),
            ]
        )
        graph = StateGraph(schema, name=law_id)

        def decide(state: dict) -> dict:
            return {"decision": gate_decision(state.get("ok") is True)}

        graph.add_node("decide", decide)
        graph.add_edge(START, "decide")
        graph.add_edge("decide", END)
        return graph

    return build


def law_specs(root: Path | None = None, measured: dict[str, list[str]] | None = None) -> list[dict]:
    if measured is None:
        measured = measure_all(root)
    rows = (
        ("lift.project_version", "version"),
        ("lift.audio_thread", "audio"),
        ("lift.shipped_names", "names"),
        ("lift.radio_client", "radio"),
        ("lift.vst3_no_radio", "vst3"),
    )
    specs = []
    for law_id, key in rows:
        specs.append(
            {
                "id": law_id,
                "build": build_gate(law_id),
                "state": {"ok": measured[key] == []},
                "allow_decisions": ["allow"],
            }
        )
    return specs
