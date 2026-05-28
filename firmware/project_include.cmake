# Runs at end of CMake configure.

# PicoTTS: -O2 maybe-uninitialized in picoos.c (warnings are errors).
execute_process(
    COMMAND python "${CMAKE_SOURCE_DIR}/tools/patch_picotts.py"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RESULT_VARIABLE _patch_picotts_result
)
if(NOT _patch_picotts_result EQUAL 0)
    message(WARNING "patch_picotts.py failed (${_patch_picotts_result})")
endif()

# esp-sr: drop unused Chinese TTS (~6 MB) — English MultiNet + PicoTTS only.
execute_process(
    COMMAND python "${CMAKE_SOURCE_DIR}/tools/patch_esp_sr_no_chinese.py"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RESULT_VARIABLE _patch_esp_sr_result
)
if(NOT _patch_esp_sr_result EQUAL 0)
    message(WARNING "patch_esp_sr_no_chinese.py failed (${_patch_esp_sr_result})")
endif()

# esp-sr movemodel.py uses Unicode box-drawing characters that crash under Windows cp1252.
if(WIN32)
    execute_process(
        COMMAND python "${CMAKE_SOURCE_DIR}/tools/patch_movemodel.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _patch_movemodel_result
    )
    if(NOT _patch_movemodel_result EQUAL 0)
        message(WARNING "patch_movemodel.py failed (${_patch_movemodel_result})")
    endif()
endif()
