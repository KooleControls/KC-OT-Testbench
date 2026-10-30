# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Derive version from git tag (v0.1.0 -> 0.1.0), fallback to 0.0.0-dev.
# A release candidate builds as the version it is a candidate FOR (v1.2.0-rc.2 ->
# 1.2.0): the final release ships the RC's binary unchanged, so the binary must
# already carry the final version. Which RC a device runs is told by the commit
# below, not by the version string.
execute_process(
    COMMAND git describe --tags --abbrev=0
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_TAG_RESULT
)
if(GIT_TAG_RESULT EQUAL 0 AND GIT_TAG MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+)-rc\\.[0-9]+$")
    set(PROJECT_VER "${CMAKE_MATCH_1}")
elseif(GIT_TAG_RESULT EQUAL 0 AND GIT_TAG MATCHES "^v(.+)$")
    set(PROJECT_VER "${CMAKE_MATCH_1}")
else()
    set(PROJECT_VER "0.0.0-dev")
endif()

# ── The commit, which the tag does not give us ────────────────
# esp_app_desc_t carries version, project_name, date/time and idf_ver but no VCS
# revision, and the tag above identifies a RELEASE rather than a build: two boards can
# both report 0.1.0 and be running different code, which is exactly what a device list
# exists to tell apart. So the SHA is read here and compiled in, and -dirty is part of
# the answer — a build with uncommitted changes is not the commit it names.
execute_process(
    COMMAND git describe --always --dirty --exclude "*"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_COMMIT_RESULT
)
if(NOT GIT_COMMIT_RESULT EQUAL 0)
    # A source drop with no .git, which is a legitimate way to build this. Empty
    # rather than "unknown": the device omits the key entirely and the relay shows a
    # blank cell, which is the truth.
    set(GIT_COMMIT "")
endif()
add_compile_definitions(STRUX_GIT_COMMIT="${GIT_COMMIT}")

# ── Per-board sdkconfig ───────────────────────────────────────
# The application's defaults plus the board's own, in that order. sdkconfig
# fragments cannot include one another, so this list is the composition point:
# the board file overlays the shared one and wins on conflicts.
#
# The board file is REQUIRED, not optional. It carries the hardware — flash
# size, flash timing, partition table, console routing, PSRAM — and a board
# that silently had none would inherit ESP-IDF's idea of those instead, which
# for flash size is 2 MB on an ESP32 and would produce a wrongly sized image
# rather than an error. There is nothing a board could leave to the defaults
# that is worth that failure mode, so a missing file is a build error naming
# what it is for. BOARD is the same cache var consumed by main/CMakeLists.txt.
if(NOT DEFINED BOARD)
    set(BOARD "pcb1245")
endif()
set(_strux_board_defaults "main/hardware/boards/${BOARD}/sdkconfig.defaults")
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${_strux_board_defaults}")
    file(GLOB _strux_boards RELATIVE "${CMAKE_SOURCE_DIR}/main/hardware/boards"
         "${CMAKE_SOURCE_DIR}/main/hardware/boards/*")
    message(FATAL_ERROR
        "Board '${BOARD}' has no ${_strux_board_defaults}.\n\n"
        "Every board declares its own hardware there — flash size and timing, the "
        "partition table it names, console routing, PSRAM. The shared "
        "sdkconfig.defaults holds what the APPLICATION needs and deliberately "
        "asserts none of those, so without this file the build would take "
        "ESP-IDF's defaults for them and produce a wrongly sized image.\n\n"
        "Available boards: ${_strux_boards}")
endif()
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults" "${_strux_board_defaults}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(KC-OT-Testbench)

# ── sdkconfig drift guard ────────────────────────────
# A generated sdkconfig is loaded AFTER the defaults and wins every conflict, and an
# option sitting at its default is still recorded there — as "# CONFIG_FOO is not set"
# — so adding CONFIG_FOO=y to sdkconfig.defaults changes NOTHING in a tree that already
# has a sdkconfig. It fails silently: the build succeeds and runs the old configuration,
# which is how a config change can look landed and do nothing. The CONFIG_IDF_TARGET
# case in main/CMakeLists.txt is this same mechanism, and the reason a wrong-toolchain
# build is possible at all.
#
# So every assertion in the composed defaults is checked against what was actually
# generated, and a mismatch is a build error naming the options that did not take. The
# fix it prints is the only fix: delete the generated file — it is gitignored and
# reproducible — and let set-target write a new one.
#
# For a deliberate local override of something the defaults assert:
#   idf.py -DSTRUX_ALLOW_SDKCONFIG_DRIFT=ON build
#
# ── What the composed defaults ASSERT is not every line in them ──
# The files above are a composition: the board overlay is loaded after the root file
# and wins, which is the entire reason a board may have one. So a key named by both is
# asserted ONCE, by the LAST file to name it, and that is what this checks — the files
# are reduced to one line per key before anything is tested.
#
# It did not always. It checked every file's line independently, which made an overlay
# able to ADD a key and never to OVERRIDE one: the first board to lower an application
# default would have failed a tree configured exactly as asked, with the root file's
# line reported as drift. The guard was wrong, not the board.
#
# There is deliberately no escape hatch for a single key. A board that has to falsify a
# root assertion rather than shadow it — Kconfig CHOICEs behave that way, since picking
# another arm sets a DIFFERENT symbol and no last-writer rule can connect the two — is
# telling you that the key is in the wrong file. Flash size is the worked example, and
# the answer is that it belongs to the board, not that the guard needs a way to be told
# to ignore it. The error below says so.
#
# Only lines of the form CONFIG_X=... are checked, which is every line either defaults
# file uses. A default whose *value* contained a semicolon would be misread, because
# file(STRINGS) hands back a CMake list; none do, and if one ever does it is this loop
# that needs fixing, not the value.
if(NOT STRUX_ALLOW_SDKCONFIG_DRIFT)
    idf_build_get_property(_strux_sdkconfig SDKCONFIG)
    file(READ "${_strux_sdkconfig}" _strux_actual)
    string(REPLACE "\r" "" _strux_actual "${_strux_actual}")  # generated file is CRLF
    set(_strux_actual "\n${_strux_actual}\n")              # so every line has both

    # ── Pass one: reduce the composed files to what they actually assert ──
    # Each key is kept in a variable named after itself, so a later file simply
    # overwrites an earlier one and the last writer wins — which is what composition
    # means, and what the overlay is for.
    set(_strux_keys "")
    foreach(_strux_file ${SDKCONFIG_DEFAULTS})
        # project.cmake has already made these absolute; BASE_DIR keeps this working
        # either way, since the list is composed with relative paths above.
        get_filename_component(_strux_path "${_strux_file}" ABSOLUTE
                               BASE_DIR "${CMAKE_SOURCE_DIR}")
        file(RELATIVE_PATH _strux_shown "${CMAKE_SOURCE_DIR}" "${_strux_path}")
        file(STRINGS "${_strux_path}" _strux_lines)
        foreach(_strux_line ${_strux_lines})
            if(NOT _strux_line MATCHES "^(CONFIG_[A-Za-z0-9_]+)=")
                continue()
            endif()
            set(_strux_key "${CMAKE_MATCH_1}")
            list(APPEND _strux_keys "${_strux_key}")
            set(_strux_want_${_strux_key} "${_strux_line}")
            set(_strux_from_${_strux_key} "${_strux_shown}")
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES _strux_keys)

    # ── Pass two: check what survived against what was generated ──
    set(_strux_drift "")
    foreach(_strux_key ${_strux_keys})
        string(FIND "${_strux_actual}" "\n${_strux_want_${_strux_key}}\n" _strux_pos)
        if(NOT _strux_pos EQUAL -1)
            continue()
        endif()
        # Say what it is instead, so the message is diagnosable without a diff.
        if("${_strux_actual}" MATCHES "\n${_strux_key}=([^\n]*)\n")
            set(_strux_is "it is ${_strux_key}=${CMAKE_MATCH_1}")
        elseif("${_strux_actual}" MATCHES "\n# ${_strux_key} is not set\n")
            set(_strux_is "it is not set")
        else()
            set(_strux_is "it is absent entirely — its dependencies are unmet")
        endif()
        list(APPEND _strux_drift
             "  ${_strux_from_${_strux_key}} asks for ${_strux_want_${_strux_key}}, but ${_strux_is}")
    endforeach()

    if(_strux_drift)
        string(REPLACE ";" "\n" _strux_drift "${_strux_drift}")
        message(FATAL_ERROR
            "${_strux_sdkconfig} disagrees with the defaults it was generated from:\n"
            "${_strux_drift}\n\n"
            "Usually this is a STALE generated file. It wins over sdkconfig.defaults, so "
            "those lines are not in effect and this build would silently use the old "
            "configuration. Delete it and let set-target rewrite it:\n"
            "  rm ${_strux_sdkconfig}\n"
            "  idf.py -DBOARD=${BOARD} set-target <chip>\n\n"
            "If the generated file is fresh, the option is in the wrong file. A board "
            "overlay OVERRIDES a root default of the same key and that is not drift — "
            "but a Kconfig choice is picked by setting a DIFFERENT symbol, so a board "
            "selecting another arm falsifies the root's line instead of shadowing it. "
            "That means the option is a property of the hardware: move it out of "
            "sdkconfig.defaults into every board's own overlay.\n\n"
            "If a difference is a deliberate local override, build with "
            "-DSTRUX_ALLOW_SDKCONFIG_DRIFT=ON.")
    endif()
endif()
