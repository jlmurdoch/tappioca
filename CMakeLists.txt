# Set minimum required version of CMake
cmake_minimum_required(VERSION 3.12)

# Include build functions from Pico SDK
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
include($ENV{PICO_SDK_PATH}/tools/CMakeLists.txt)

project(tappioca C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# add_subdirectory(FatFs_SPI build)

add_executable(tappioca 
	main.c
#    hw_config.c
#    inflate.c
)

# by default the header is generated into the build dir
pico_generate_pio_header(tappioca ${CMAKE_CURRENT_LIST_DIR}/pulsegen.pio)

target_sources(tappioca PRIVATE main.c)

target_link_libraries(tappioca PRIVATE 
    pico_stdlib 
    hardware_pio
#    FatFs_SPI
)

pico_enable_stdio_usb(tappioca 1)
pico_enable_stdio_uart(tappioca 0)
pico_add_extra_outputs(tappioca)
