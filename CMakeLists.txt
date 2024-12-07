# Set minimum required version of CMake
cmake_minimum_required(VERSION 3.12)

# Include build functions from Pico SDK
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
include($ENV{PICO_SDK_PATH}/tools/CMakeLists.txt)

project(tappioca C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# -DUSE_FATFS=ON -USE_ZLIB=ON
option(USE_FATFS, "Use FatFS" OFF)
option(USE_ZLIB, "Use Zlib" OFF)

if(USE_FATFS)
    add_subdirectory(no-OS-FatFS-SD-SPI-RPi-Pico/FatFs_SPI)
endif(USE_FATFS)

add_executable(tappioca)

# by default the header is generated into the build dir
pico_generate_pio_header(tappioca ${CMAKE_CURRENT_LIST_DIR}/pulsegen.pio)

target_sources(tappioca PRIVATE main.c)

if(USE_FATFS)
    target_compile_definitions(tappioca PRIVATE USE_FATFS=1)
    target_sources(tappioca PRIVATE hw_config.c)
endif(USE_FATFS)
if(USE_ZLIB)
    target_compile_definitions(tappioca PRIVATE USE_ZLIB=1)
    target_sources(tappioca PRIVATE zlib-deflate-nostdlib/src/inflate.c)
endif(USE_ZLIB)

target_link_libraries(tappioca PRIVATE pico_stdlib hardware_pio)

if(USE_FATFS)
    target_link_libraries(tappioca PRIVATE FatFs_SPI)
endif(USE_FATFS)

pico_enable_stdio_usb(tappioca 1)
pico_enable_stdio_uart(tappioca 0)
pico_add_extra_outputs(tappioca)
