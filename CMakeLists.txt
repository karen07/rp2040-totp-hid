cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(rp2040_totp_hid C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(rp2040_totp_hid src/main.c src/usb_descriptors.c)

target_include_directories(rp2040_totp_hid
                           PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)

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
