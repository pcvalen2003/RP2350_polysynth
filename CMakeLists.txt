# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2 CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(PicoFX C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(PicoFX PicoFX.c
    usb_descriptors.c
    synth.c
    ssd1306.c
    )

pico_set_program_name(PicoFX "PicoFX")
pico_set_program_version(PicoFX "0.1")


# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(PicoFX 0)
pico_enable_stdio_usb(PicoFX 0)

# Generar headers pio
pico_generate_pio_header(PicoFX ${CMAKE_CURRENT_LIST_DIR}/audio_i2s.pio)

# Add the standard library to the build
target_link_libraries(PicoFX
        pico_stdlib
        tinyusb_device # agrego el tiny USB
        tinyusb_board
        hardware_pio
        hardware_dma
        hardware_clocks
        hardware_i2c       # i2c_init y I2C0
        hardware_gpio      # Para configurar el Pin Mux (GP4/GP5)
        pico_multicore
    )

# Add the standard include files to the build
target_include_directories(PicoFX PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

pico_add_extra_outputs(PicoFX)

