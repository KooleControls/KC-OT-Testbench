# ── The substrate every layer stands on: RTOS wrappers, streams, JSON, the
# request/reply seam, date and time. NOT part of the framework — the board layer uses
# InitState and the application uses Timer, and both would be reaching inside strux/ to
# get them if this lived there. Nothing in here knows about a layer; that is the test
# for whether a thing belongs.
set(LIB_SOURCES
    "lib/system/DateTime.cpp"
    "lib/system/TimeSpan.cpp"
)

# ── The framework. Pulled from the Strux template; a fork should not need to touch
# this list, which is why it is kept apart from the application's.
set(STRUX_SOURCES
    "strux/NetworkManager/NetworkManager.cpp"
    "strux/NetworkManager/NetworkInterface.cpp"
    "strux/NetworkManager/WiFiInterface.cpp"
    "strux/NetworkManager/EthernetInterface.cpp"
    "strux/NetworkManager/DnsConfiguration.cpp"
    "strux/WebAssets/WebAssets.cpp"
    "strux/WebServerManager/WebServerManager.cpp"
    "strux/WebServerManager/StaticFileHandler.cpp"
    "strux/WebServerManager/WebSocketHandler.cpp"
    "strux/WebServerManager/ConnectionRegistry.cpp"
    "strux/AuthManager/AuthManager.cpp"
    "strux/AuthManager/Authenticator.cpp"
    "strux/AuthManager/ResumeTokens.cpp"
    "strux/RelayManager/RelayManager.cpp"
    "strux/RelayManager/RelaySocket.cpp"
    "strux/TelemetryManager/TelemetryManager.cpp"
    "strux/CommandManager/CommandManager.cpp"
    "strux/PartitionManager/PartitionManager.cpp"
    "strux/PartitionManager/PartitionWriter.cpp"
    "strux/SettingsManager/SettingsManager.cpp"
    "strux/SettingsManager/Setting.cpp"
    "strux/SystemManager/SystemManager.cpp"
    "strux/ConsoleManager/ConsoleManager.cpp"
    "strux/TimeManager/TimeManager.cpp"
)

# ── The application. This is the list a fork owns.
set(APP_SOURCES
    "app/LedManager/LedManager.cpp"
    "app/ModuleFirmwareManager/ModuleFirmwareManager.cpp"
    "app/ModuleLinkManager/ModuleLinkManager.cpp"
    "app/RoutingManager/RoutingManager.cpp"
)

set(SOURCE_FILES_LIST
    "main.cpp"
    ${LIB_SOURCES}
    ${STRUX_SOURCES}
    ${APP_SOURCES}
)

# Every folder is on the include path, so headers are included by name alone
# (`#include "Stream.h"`) and moving one between layers does not touch its callers.
set(INCLUDE_DIRS_LIST
    .
    "lib"
    "lib/common"
    "lib/rtos"
    "lib/json"
    "lib/protocol"
    "lib/system"
    "strux"
    "strux/WebAssets"
    "strux/NetworkManager"
    "strux/WebServerManager"
    "strux/AuthManager"
    "strux/RelayManager"
    "strux/TelemetryManager"
    "strux/CommandManager"
    "strux/PartitionManager"
    "strux/SettingsManager"
    "strux/SystemManager"
    "strux/ConsoleManager"
    "strux/TimeManager"
    "hardware"
    "app"
    "app/LedManager"
    "app/ModuleFirmwareManager"
    "app/ModuleLinkManager"
    "app/RoutingManager"
)

set(COMPONENT_REQUIRES
    esp_timer
    esp_netif
    esp_event
    esp_wifi
    nvs_flash
    esp_http_server
    esp_http_client
    esp_app_format
    app_update
    esp_partition
    spi_flash
    esp_driver_gpio
    # PCB1245: the MAX14830 expander, and the ESP32's Ethernet MAC.
    esp_driver_spi
    esp_eth
    mdns
    # Only to report the DFS bounds in `system info` — the scaling itself is
    # configured by startup code, see CONFIG_PM_ENABLE in sdkconfig.defaults.
    esp_pm
    # The relay drives a WebSocket at the transport layer instead of through
    # esp_websocket_client — see RelaySocket.h for why. esp-tls comes with it for
    # the certificate bundle a wss:// pipe verifies against.
    tcp_transport
    esp-tls
)

# ── Board selection ────────────────────────────────────────────
# Pick the target board with -DBOARD=<name>; defaults to the PCB1245
# gateway. Each board lives in main/hardware/boards/<name>/ with its own
# BoardConfig.h and a board.cmake fragment that may append extra files to
# BOARD_SOURCES. Shared, board-independent drivers live in hardware/drivers/.
set(BOARD "pcb1245" CACHE STRING "Target board (folder under main/hardware/boards/)")

set(BOARD_DIR "${CMAKE_CURRENT_LIST_DIR}/hardware/boards/${BOARD}")
if(NOT EXISTS "${BOARD_DIR}/board.cmake")
    file(GLOB _available_boards RELATIVE "${CMAKE_CURRENT_LIST_DIR}/hardware/boards"
         "${CMAKE_CURRENT_LIST_DIR}/hardware/boards/*")
    message(FATAL_ERROR "Unknown BOARD '${BOARD}'. Available boards: ${_available_boards}")
endif()

set(BOARD_SOURCES "")
include("${BOARD_DIR}/board.cmake")

# NOTE on component REQUIRES: ESP-IDF resolves a component's REQUIRES in a
# separate "expansion" pass that runs this CMakeLists in script mode WITHOUT
# the -DBOARD cache var, so board.cmake can't switch REQUIRES per board (it
# would always see the default board). Component deps therefore go through the
# two channels that DO work regardless of board:
#   • managed deps -> main/idf_component.yml
#   • IDF built-ins -> COMPONENT_REQUIRES above
# board.cmake is limited to BOARD_SOURCES, which is evaluated in the normal
# configure pass where BOARD is visible.

# The selected board folder is the ONLY one on the include path, so headers
# included by name (e.g. BoardConfig.h) resolve to it.
list(APPEND INCLUDE_DIRS_LIST "hardware/boards/${BOARD}")
list(APPEND SOURCE_FILES_LIST ${BOARD_SOURCES})
message(STATUS "Building for BOARD=${BOARD}")

# ── The embedded frontend ─────────────────────────────────────
# The built frontend does not live on a partition of its own: it is packed into
# one blob and linked into the app image, so a firmware image is the whole
# product and an OTA carries the UI with the code that serves it.
#
# Chain:  frontend/src/**  ->  (pnpm build)  ->  www/
#         www/             ->  (pack_web_assets.py)  ->  web_assets.bin
#         web_assets.bin   ->  (EMBED_FILES)  ->  rodata in the app image
#
# It is one blob rather than one EMBED_FILES entry per asset because vite
# content-hashes its filenames, and EMBED_FILES needs its list when CMake
# configures — long before vite has run. A single fixed name sidesteps that
# entirely, and the blob's directory is the manifest WebAssets.cpp reads.
idf_build_get_property(_python PYTHON)

set(FRONTEND_DIR   "${CMAKE_CURRENT_LIST_DIR}/../frontend")
set(WWW_DIR        "${CMAKE_CURRENT_LIST_DIR}/../www")
set(PACK_SCRIPT    "${CMAKE_CURRENT_LIST_DIR}/strux/WebAssets/pack_web_assets.py")
set(WEB_ASSETS_BIN "${CMAKE_CURRENT_BINARY_DIR}/web_assets.bin")
set(FRONTEND_STAMP "${CMAKE_CURRENT_BINARY_DIR}/frontend.stamp")

# ── The PCB1246 firmware ──────────────────────────────────────
# The OpenTherm module's .bin rides inside this app image, so one OTA updates both
# boards: ModuleFirmwareManager flashes the module with it over ISP whenever the module
# runs anything else. Built first, by the opentherm/ project next to this one.
#
# Copied to a fixed name so the linker symbols are fixed too
# (_binary_pcb1246_bin_start). configure_file makes the copy a configure dependency,
# so rebuilding the LPC and then the gateway picks the new image up.
#
# Missing is a warning for a developer build -- the gateway then leaves the module as
# it is -- and an error when PCB1246_FIRMWARE_REQUIRED is set, which the release
# workflow does: a release that silently stopped carrying the module's firmware would
# look fine until a board needed it.
set(PCB1246_FIRMWARE "${CMAKE_CURRENT_LIST_DIR}/../../opentherm/build/opentherm.bin"
    CACHE FILEPATH "The PCB1246 firmware image to embed")
option(PCB1246_FIRMWARE_REQUIRED "Fail the build when the PCB1246 image is missing" OFF)

set(PCB1246_EMBED "")
if(EXISTS "${PCB1246_FIRMWARE}")
    set(PCB1246_EMBED "${CMAKE_CURRENT_BINARY_DIR}/pcb1246.bin")
    if(NOT CMAKE_BUILD_EARLY_EXPANSION)
        configure_file("${PCB1246_FIRMWARE}" "${PCB1246_EMBED}" COPYONLY)
    endif()
elseif(PCB1246_FIRMWARE_REQUIRED)
    message(FATAL_ERROR "PCB1246 firmware not found at ${PCB1246_FIRMWARE} - build opentherm/ first")
elseif(NOT CMAKE_BUILD_EARLY_EXPANSION)
    message(WARNING "PCB1246 firmware not found at ${PCB1246_FIRMWARE} - this build will not update the module")
endif()

# Register component with ESP-IDF
idf_component_register(
    SRCS ${SOURCE_FILES_LIST}
    INCLUDE_DIRS ${INCLUDE_DIRS_LIST}
    REQUIRES ${COMPONENT_REQUIRES}
    EMBED_FILES ${WEB_ASSETS_BIN} ${PCB1246_EMBED}
)

if(PCB1246_EMBED)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PCB1246_FIRMWARE_EMBEDDED=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PCB1246_FIRMWARE_EMBEDDED=0)
endif()

# Board identity define for the (rare) code-level conditionals.
string(TOUPPER "${BOARD}" BOARD_UPPER)
target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_${BOARD_UPPER})

# Switches over enums (e.g. SettingType) deliberately have no default case
# so that adding an enum value flags every converter that wasn't updated.
# That flag is only a warning by default — make it the error it's meant to be.
target_compile_options(${COMPONENT_LIB} PRIVATE -Werror=switch)

# ── Building and packing the frontend ─────────────────────────
# Everything below runs only in the second pass — idf_component_register returns
# early while ESP-IDF is still collecting requirements (the REQUIRES note above),
# and registering these rules twice would be an error.

# What a frontend rebuild depends on. CONFIGURE_DEPENDS re-runs the glob when
# ninja checks the build, so a newly added .tsx is picked up without a manual
# reconfigure. node_modules is not globbed: pnpm owns it, and walking it would
# cost more than the whole build.
file(GLOB_RECURSE FRONTEND_SOURCES CONFIGURE_DEPENDS "${FRONTEND_DIR}/src/*")
list(APPEND FRONTEND_SOURCES
    "${FRONTEND_DIR}/index.html"
    "${FRONTEND_DIR}/package.json"
    "${FRONTEND_DIR}/vite.config.ts")

find_program(PNPM pnpm)
if(PNPM)
    # --no-frozen-lockfile lets a package.json edit update the lockfile rather
    # than failing a firmware build; --config.confirm-modules-purge=false skips
    # the prompt pnpm shows when node_modules is out of sync, which would
    # otherwise hang a build with nobody watching.
    add_custom_command(OUTPUT "${FRONTEND_STAMP}"
        COMMAND "${PNPM}" install --no-frozen-lockfile --config.confirm-modules-purge=false
        COMMAND "${PNPM}" build
        COMMAND "${CMAKE_COMMAND}" -E touch "${FRONTEND_STAMP}"
        WORKING_DIRECTORY "${FRONTEND_DIR}"
        DEPENDS ${FRONTEND_SOURCES}
        COMMENT "Building frontend (pnpm install && pnpm build)"
        VERBATIM USES_TERMINAL)
else()
    # A device must still build on a machine without node. The packer falls back
    # to a placeholder page, so the firmware always has a valid blob and no code
    # anywhere has to handle a missing one.
    # The stamp follows www/ instead, so a frontend built by hand is still packed;
    # with no dependencies at all it was packed once and then never again.
    message(STATUS "pnpm not found — the frontend will not be rebuilt, www/ is packed as it is")
    file(GLOB_RECURSE WWW_FILES CONFIGURE_DEPENDS "${WWW_DIR}/*")
    add_custom_command(OUTPUT "${FRONTEND_STAMP}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${FRONTEND_STAMP}"
        DEPENDS ${WWW_FILES}
        VERBATIM)
endif()

# The packer's output is what EMBED_FILES names, so this rule is what teaches
# CMake how to produce it — the embed step depends on the file, this rule makes
# it, and the stamp in between forces the frontend to build first.
add_custom_command(OUTPUT "${WEB_ASSETS_BIN}"
    COMMAND "${_python}" "${PACK_SCRIPT}" --src "${WWW_DIR}" --out "${WEB_ASSETS_BIN}"
    DEPENDS "${FRONTEND_STAMP}" "${PACK_SCRIPT}"
    COMMENT "Packing web assets into web_assets.bin"
    VERBATIM)
