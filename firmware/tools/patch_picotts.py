"""Fix PicoTTS picoos.c -O2 maybe-uninitialized warning (treated as error)."""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PICOOS = ROOT / "managed_components" / "jmattsson__picotts" / "pico" / "lib" / "picoos.c"
CMAKE = ROOT / "managed_components" / "jmattsson__picotts" / "CMakeLists.txt"

OLD_DECL = "    picoos_char b;"
NEW_DECL = "    picoos_char b = 0;"

WARN_FLAGS = (
    '"-Wno-implicit-fallthrough -Wno-unused-but-set-variable -Wno-unused-function"'
)
WARN_FLAGS_PATCHED = (
    '"-Wno-implicit-fallthrough -Wno-unused-but-set-variable -Wno-unused-function '
    '-Wno-maybe-uninitialized"'
)


def main() -> int:
    changed = False

    if PICOOS.is_file():
        text = PICOOS.read_text(encoding="utf-8")
        if NEW_DECL in text:
            pass
        elif OLD_DECL in text:
            PICOOS.write_text(text.replace(OLD_DECL, NEW_DECL, 1), encoding="utf-8")
            print(f"Patched {PICOOS}")
            changed = True
        else:
            print(f"patch_picotts: picoos.c pattern missing in {PICOOS}")
            return 1

    if CMAKE.is_file():
        cmake = CMAKE.read_text(encoding="utf-8")
        if WARN_FLAGS_PATCHED not in cmake and WARN_FLAGS in cmake:
            cmake = cmake.replace(WARN_FLAGS, WARN_FLAGS_PATCHED, 1)
            CMAKE.write_text(cmake, encoding="utf-8")
            print(f"Patched {CMAKE}: suppress maybe-uninitialized")
            changed = True

    if not changed:
        return 0

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
