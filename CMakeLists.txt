cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(
    rp2040_totp_hid
    VERSION 1.1.0
    LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(rp2040_totp_hid src/main.c src/usb_descriptors.c)

target_include_directories(rp2040_totp_hid
                           PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)

target_compile_definitions(
    rp2040_totp_hid
    PRIVATE FW_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
            FW_VERSION_MINOR=${PROJECT_VERSION_MINOR}
            FW_VERSION_PATCH=${PROJECT_VERSION_PATCH} WEBHID_PROTOCOL_VERSION=1)

target_link_libraries(
    rp2040_totp_hid
    PRIVATE pico_stdlib
            pico_unique_id
            hardware_i2c
            hardware_gpio
            hardware_pio
            hardware_uart
            hardware_sync
            tinyusb_device
            tinyusb_board)

pico_generate_pio_header(rp2040_totp_hid
                         ${CMAKE_CURRENT_LIST_DIR}/src/ws2812.pio)

target_compile_options(rp2040_totp_hid PRIVATE -Wall -Wextra -Werror)

pico_add_extra_outputs(rp2040_totp_hid)
