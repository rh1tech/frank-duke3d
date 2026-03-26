cmake_minimum_required(VERSION 3.13)

# Set board to Pico 2 (RP2350)
set(PICO_BOARD pico2 CACHE STRING "Pico board type")

# Board variant (M1, M2)
set(BOARD_VARIANT "M1" CACHE STRING "Board variant: M1 or M2")

# CPU overclock configuration (252, 378, 504 MHz)
# 252 = no overclock (default), 378 = medium, 504 = high (Quake port default)
set(CPU_SPEED "252" CACHE STRING "CPU clock in MHz: 252, 378, 504")

# PSRAM speed configuration (84, 100, 133, 166 MHz target)
# Actual speed depends on CPU clock divider
set(PSRAM_SPEED "133" CACHE STRING "PSRAM max frequency in MHz: 84, 100, 133, 166")

# Flash chip speed configuration (default 66 MHz)
# Lower values improve stability, higher values improve performance
set(FLASH_SPEED "66" CACHE STRING "Flash chip max frequency in MHz: 66, 88")

# USB HID configuration
# When enabled: USB port is used for keyboard/mouse input, debug output via UART
# When disabled (default): USB port is used for serial console debug output
option(USB_HID_ENABLED "Enable USB HID keyboard/mouse support (disables USB serial console)" OFF)

# CPU voltage selection based on speed
# Higher speeds need higher voltage for stability
if(CPU_SPEED GREATER_EQUAL 504)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_65")
elseif(CPU_SPEED GREATER_EQUAL 378)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_60")
else()
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_50")
endif()
message(STATUS "CPU voltage: ${CPU_VOLTAGE}")

# Board-specific GPIO pins
if(BOARD_VARIANT STREQUAL "M1")
    # M1 SD Card pins
    set(SD_PIN_SCK 2)
    set(SD_PIN_MOSI 3)
    set(SD_PIN_MISO 4)
    set(SD_PIN_CS 5)
    # M1 HDMI base pin (CLK-)
    set(HDMI_BASE_PIN 6)
    # M1 PS/2 pins
    set(PS2_CLK_PIN 0)
    set(PS2_DATA_PIN 1)
    # M1 I2S pins
    set(I2S_DATA_PIN 26)
    set(I2S_CLK_PIN 27)
else()
    # M2 SD Card pins
    set(SD_PIN_SCK 6)
    set(SD_PIN_MOSI 7)
    set(SD_PIN_MISO 4)
    set(SD_PIN_CS 5)
    # M2 HDMI base pin (CLK-)
    set(HDMI_BASE_PIN 12)
    # M2 PS/2 pins
    set(PS2_CLK_PIN 2)
    set(PS2_DATA_PIN 3)
    # M2 I2S pins
    set(I2S_DATA_PIN 9)
    set(I2S_CLK_PIN 10)
endif()

message(STATUS "Board: ${BOARD_VARIANT}, CPU: ${CPU_SPEED} MHz, PSRAM: ${PSRAM_SPEED} MHz, Flash: ${FLASH_SPEED} MHz")
message(STATUS "SD pins: SCK=${SD_PIN_SCK} MOSI=${SD_PIN_MOSI} MISO=${SD_PIN_MISO} CS=${SD_PIN_CS}")
message(STATUS "HDMI base: ${HDMI_BASE_PIN}, PS/2: CLK=${PS2_CLK_PIN} DATA=${PS2_DATA_PIN}")

# Make PS2 pins available to subdirectories
set(PS2_CLK_PIN ${PS2_CLK_PIN} CACHE INTERNAL "PS/2 Clock Pin")

include(pico_sdk_import.cmake)


project(frank-duke3d C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Read version from version.txt
file(READ "${CMAKE_CURRENT_LIST_DIR}/version.txt" FRANKDUKE_VERSION_RAW)
string(STRIP "${FRANKDUKE_VERSION_RAW}" FRANKDUKE_VERSION_RAW)
string(REGEX REPLACE "[ \t]+" "." FRANKDUKE_VERSION "${FRANKDUKE_VERSION_RAW}")
message(STATUS "Version: ${FRANKDUKE_VERSION}")

pico_sdk_init()


# Add fatfs
add_subdirectory(src/fatfs)
# Add drivers
add_subdirectory(drivers/sdcard)
add_subdirectory(drivers/ps2)
add_subdirectory(drivers/usbhid)

# Create a library for other drivers
add_library(drivers
    drivers/HDMI.c
    drivers/psram_init.c
    drivers/psram_allocator.c
)
target_include_directories(drivers PUBLIC drivers)
target_link_libraries(drivers pico_stdlib hardware_dma hardware_pio hardware_spi)
target_compile_definitions(drivers PUBLIC
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    HDMI_BASE_PIN=${HDMI_BASE_PIN}
)

# Duke3D Engine sources
set(ENGINE_SOURCES
    components/Engine/cache.c
    components/Engine/display.c
    components/Engine/draw.c
    components/Engine/engine.c
    components/Engine/filesystem.c
    components/Engine/fixedPoint_math.c
    components/Engine/network.c
    components/Engine/tiles.c
    components/Engine/mmulti.c
)

# Duke3D Game sources  
set(GAME_SOURCES
    components/Game/actors.c
    components/Game/animlib.c
    components/Game/config.c
    components/Game/console.c
    components/Game/control.c
    components/Game/cvar_defs.c
    components/Game/cvars.c
    components/Game/game.c
    components/Game/gamedef.c
    components/Game/global.c
    components/Game/keyboard.c
    components/Game/menues.c
    components/Game/player.c
    components/Game/premap.c
    components/Game/rts.c
    components/Game/scriplib.c
    components/Game/sector.c
    components/Game/sounds.c
)

# Sound system - I2S audio driver
set(SOUND_SOURCES
    src/audio.c
    src/i_picosound.c
    src/audio_stub.c
    src/i_music.c
)

# OPL Music system - emu8950 emulator and MIDI parser
set(OPL_SOURCES
    src/opl/emu8950.c
    src/opl/emuadpcm.c
    src/opl/midifile.c
)

add_executable(frank-duke3d
    src/main.c
    src/welcome.c
    src/duke3d_rp2350.c
    src/compat.c
    src/psram_data.c
    src/anim_streaming.c
    src/SDL/SDL_rp2350.c
    src/SDL/SDL_video_rp2350.c
    src/SDL/SDL_event_rp2350.c
    src/SDL/SDL_audio_stub.c
    src/fatfs_stdio.c
    ${ENGINE_SOURCES}
    ${GAME_SOURCES}
    ${SOUND_SOURCES}
    ${OPL_SOURCES}
)

# PIO header generation must come after add_executable
add_subdirectory(drivers/audio)

target_include_directories(frank-duke3d PRIVATE
    src
    src/SDL
    src/fatfs
    src/freertos
    src/opl
    components/Engine
    components/Game
    components/audiolib
    drivers
    drivers/sdcard
    drivers/ps2
)

# Only include usbhid directory when USB HID is enabled (to avoid tusb_config.h conflict)
if(USB_HID_ENABLED)
    target_include_directories(frank-duke3d PRIVATE drivers/usbhid)
endif()

target_compile_definitions(frank-duke3d PRIVATE
    BOARD_${BOARD_VARIANT}
    DBOARD_VARIANT="${BOARD_VARIANT}"
    FRANKDUKE_VERSION="${FRANKDUKE_VERSION}"
    # CPU/PSRAM/Flash speed configuration
    CPU_CLOCK_MHZ=${CPU_SPEED}
    CPU_VOLTAGE=${CPU_VOLTAGE}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    FLASH_MAX_FREQ_MHZ=${FLASH_SPEED}
    PLATFORM_SUPPORTS_SDL
    DUKE3D_RP2350
    PLATFORM_ESP32  # Use ESP32 platform code paths
    # Screen resolution
    DUKE3D_RESX=320
    DUKE3D_RESY=200
    # Platform defines
    PICO_ON_DEVICE=1
    PICO_BOARD
    # Memory management - large arrays in PSRAM
    RP2350_PSRAM
    EXT_RAM_ATTR=
    # I2S Audio Configuration
    # NOTE: Resource allocation:
    # - PIO0 SM0: PS/2 keyboard (claims first unused)
    # - PIO0 SM1: I2S audio (claims unused SM on PIO0)
    # - PIO1 SM0-1: HDMI video (claims unused)
    # - DMA IRQ0: HDMI (exclusive handler)
    # - DMA IRQ1: I2S audio (exclusive handler)
    # - DMA channels 10/11: I2S audio ping-pong
    NUM_SOUND_CHANNELS=8
    # OPL emulator configuration
    USE_EMU8950_OPL=1
    EMU8950_NO_TIMER=1
    EMU8950_NO_RATECONV=1
    EMU8950_SLOT_RENDER=0
)


# Relax some warnings for the Duke3D engine code (32-bit pointer issues)
target_compile_options(frank-duke3d PRIVATE
    -Wno-int-conversion
    -Wno-pointer-to-int-cast
    -Wno-int-to-pointer-cast
    -Wno-overflow
    -Wno-incompatible-pointer-types
)

# Allow multiple definitions (ESP32 port has variables defined in headers)
# This is a workaround - ideally headers should use extern
target_link_options(frank-duke3d PRIVATE
    -Wl,--allow-multiple-definition
)

# Use custom linker script with PSRAM sections
pico_set_linker_script(frank-duke3d ${CMAKE_CURRENT_SOURCE_DIR}/memmap_psram.ld)

target_link_libraries(frank-duke3d
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_spi
    hardware_dma
    hardware_pio
    hardware_pwm
    hardware_irq
    hardware_interp
    sdcard
    ps2_driver
    usbhid
    fatfs
    drivers
)

# Wrap stdio functions to use FatFS
target_link_options(frank-duke3d PRIVATE
    -Wl,--wrap=fopen
    -Wl,--wrap=fclose
    -Wl,--wrap=fread
    -Wl,--wrap=fgetc
    -Wl,--wrap=fwrite
    -Wl,--wrap=fseek
    -Wl,--wrap=ftell
    -Wl,--wrap=remove
    -Wl,--wrap=rename
    -Wl,--wrap=open
    -Wl,--wrap=close
    -Wl,--wrap=read
    -Wl,--wrap=write
    -Wl,--wrap=lseek
)

# Configure stdio based on USB HID setting
# When USB HID enabled: USB port is used for HID, debug via UART
# When USB HID disabled: USB port is used for serial console
if(USB_HID_ENABLED)
    message(STATUS "Debug output: UART (USB used for HID)")
    pico_enable_stdio_usb(frank-duke3d 0)
    pico_enable_stdio_uart(frank-duke3d 1)
else()
    message(STATUS "Debug output: USB Serial Console")
    pico_enable_stdio_usb(frank-duke3d 1)
    pico_enable_stdio_uart(frank-duke3d 0)
endif()

pico_add_extra_outputs(frank-duke3d)
