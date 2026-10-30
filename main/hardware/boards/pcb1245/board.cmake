# ──────────────────────────────────────────────────────────────
# Board fragment: PCB1245
#   The KC1245 gateway · ESP32 (Xtensa), 16 MB flash, PSRAM · MAX14830 · Ethernet
#
# A board fragment may append to BOARD_SOURCES (extra .cpp files under this
# folder that need compiling). Component deps are NOT set here — see the note
# in main/CMakeLists.txt: managed deps go in main/idf_component.yml, IDF
# built-ins in COMPONENT_REQUIRES.
# ──────────────────────────────────────────────────────────────

set(_drivers "${CMAKE_CURRENT_LIST_DIR}/../../drivers")

list(APPEND BOARD_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/BoardContext.cpp"
    "${_drivers}/Max14830/Max14830.cpp"
    "${_drivers}/Max14830/Max14830Uart.cpp"
    "${_drivers}/Esp32Ethernet.cpp"
    "${_drivers}/LpcIsp/LpcIsp.cpp"
)
