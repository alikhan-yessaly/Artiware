# bleterm: C-level BLE terminal for the Pico W BLE IDE.
add_library(usermod_bleterm INTERFACE)

target_sources(usermod_bleterm INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/bleterm.c
    ${CMAKE_CURRENT_LIST_DIR}/bleterm_led.c
    ${CMAKE_CURRENT_LIST_DIR}/bleterm_nus.c
    ${CMAKE_CURRENT_LIST_DIR}/bleterm_pbl.c
    ${CMAKE_CURRENT_LIST_DIR}/pbl_codec.c
)

target_include_directories(usermod_bleterm INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(usermod INTERFACE usermod_bleterm)
