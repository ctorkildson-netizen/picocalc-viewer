# The PicoCalc picture viewer, for an RP2350 (Pico 2 / Pico 2 W).
# Blair Leduc's drivers and shell header, and the picker and picture
# decoders from picocalc-extras, are submodules in modules/:
#     git submodule update --init --recursive
cmake_minimum_required(VERSION 3.13)
set(CMAKE_C_STANDARD 11)
set(PICO_BOARD pico2_w CACHE STRING "Board type")
include(pico_sdk_import.cmake)
project(picocalc-viewer C CXX ASM)
pico_sdk_init()

set(MODULES ${CMAKE_CURRENT_LIST_DIR}/modules)
set(DRV ${MODULES}/picocalc-text-starter/drivers)
include(${MODULES}/picocalc-extras/picker/picker.cmake)
include(${MODULES}/picocalc-extras/images/images.cmake)
include(${MODULES}/picocalc-extras/fatfs/fatfs.cmake)
picocalc_fatfs(${MODULES}/picocalc-text-starter)	# the SD card, with FatFs

add_executable(viewer
    src/main.c
    ${DRV}/font-5x10.c ${DRV}/font-8x10.c
    ${DRV}/keyboard.c ${DRV}/lcd.c ${DRV}/southbridge.c
)
target_include_directories(viewer PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${MODULES}/picocalc-text-starter
    ${DRV}
    ${MODULES}/picocalc-shell/include)
target_compile_definitions(viewer PRIVATE PICO_MALLOC_PANIC=0)
pico_set_program_name(viewer "viewer")
pico_enable_stdio_uart(viewer 0)
pico_enable_stdio_usb(viewer 0)
target_link_libraries(viewer picocalc_picker picocalc_images picocalc_fs
    pico_stdlib hardware_gpio hardware_i2c hardware_spi
    hardware_pio hardware_watchdog hardware_vreg hardware_clocks pico_multicore)
pico_add_extra_outputs(viewer)
