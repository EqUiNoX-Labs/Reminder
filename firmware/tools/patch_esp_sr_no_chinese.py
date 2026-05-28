"""Drop unused Chinese ESP-TTS libraries from esp-sr (English SR + PicoTTS only)."""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CMAKE = ROOT / "managed_components" / "espressif__esp-sr" / "CMakeLists.txt"
MARKER = "# Reminder: omit Chinese TTS libs"


def main() -> int:
    if not CMAKE.is_file():
        return 0

    text = CMAKE.read_text(encoding="utf-8")
    if MARKER in text:
        return 0

    replacements = [
        (
            '    set(include_dirs\n'
            '        "esp-tts/esp_tts_chinese/include"\n'
            '        "include/${TARGET_LIB_PATH}"\n',
            '    set(include_dirs\n'
            '        "include/${TARGET_LIB_PATH}"\n',
        ),
        (
            '    target_link_libraries(${COMPONENT_TARGET} "-L \\"${CMAKE_CURRENT_SOURCE_DIR}/esp-tts/esp_tts_chinese/${IDF_TARGET}\\"")\n',
            f'    {MARKER}\n',
        ),
        (
            '    add_prebuilt_library(esp_tts_chinese "${CMAKE_CURRENT_SOURCE_DIR}/esp-tts/esp_tts_chinese/${IDF_TARGET}/libesp_tts_chinese.a" PRIV_REQUIRES ${COMPONENT_NAME})\n'
            '    add_prebuilt_library(voice_set_xiaole "${CMAKE_CURRENT_SOURCE_DIR}/esp-tts/esp_tts_chinese/${IDF_TARGET}/libvoice_set_xiaole.a" PRIV_REQUIRES ${COMPONENT_NAME})\n',
            '',
        ),
        (
            '        esp_audio_processor\n'
            '        esp_tts_chinese\n'
            '        voice_set_xiaole\n'
            '        fst\n',
            '        esp_audio_processor\n'
            '        fst\n',
        ),
    ]

    for old, new in replacements:
        if old not in text:
            print(f"patch_esp_sr_no_chinese: pattern missing in {CMAKE}, esp-sr version changed?")
            return 1
        text = text.replace(old, new, 1)

    CMAKE.write_text(text, encoding="utf-8")
    print(f"Patched {CMAKE}: removed Chinese ESP-TTS linkage")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
