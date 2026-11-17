add_subdirectory(
    ${CMAKE_CURRENT_LIST_DIR}/st7789
    ${CMAKE_BINARY_DIR}/st7789
)

# BLE REPL terminal (from picow-ble-ide firmware/modules/bleterm). Needs the
# MicroPython patches in patches/micropython-v1.29.0.
include(${CMAKE_CURRENT_LIST_DIR}/bleterm/micropython.cmake)
