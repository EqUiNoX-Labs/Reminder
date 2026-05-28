"""Patch esp-sr movemodel.py for Windows cp1252 consoles (ASCII-only report lines)."""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MOVEMODEL = ROOT / "managed_components" / "espressif__esp-sr" / "model" / "movemodel.py"

OLD = "print(u'─' * 40)"
NEW = "print('-' * 40)"


def main() -> int:
    if not MOVEMODEL.is_file():
        return 0

    text = MOVEMODEL.read_text(encoding="utf-8")
    if OLD not in text:
        return 0

    MOVEMODEL.write_text(text.replace(OLD, NEW), encoding="utf-8")
    print(f"Patched {MOVEMODEL} for Windows console encoding")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
