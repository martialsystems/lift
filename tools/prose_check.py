# Copyright (c) 2026 Martial Systems LLC. All rights reserved.
"""README and LICENSE only. The five product fences are GraphForge."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PAGES = ("README.md", "LICENSE")
COPYRIGHT = "Copyright"
BANNED = ("\u2014", "\u2013", "What it is not", "What this is not", "What it is NOT")


def main() -> int:
    failed = False
    for name in PAGES:
        text = (ROOT / name).read_text(encoding="utf-8")
        if COPYRIGHT not in text or "Martial Systems LLC" not in text:
            print(f"{name}: missing copyright")
            failed = True
        for token in BANNED:
            if token in text:
                print(f"{name}: found {token!r}")
                failed = True
    if failed:
        return 1
    print("ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
