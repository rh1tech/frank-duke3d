/*
 * Duke Nukem 3D - RP2350 Port
 * Main entry point with Welcome Screen
 */
#include "pico/stdlib.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include <stdio.h>
#include <string.h>

#include "psram_init.h"
#include "psram_data.h"
#include "psram_sections.h"
#include "board_config.h"
#include "welcome.h"

// Forward declaration of Duke3D main
extern int main_duke3d(int argc, char *argv[]);

// Game directory setter
extern void set_game_dir(const char *dir);

// Flash timing configuration for overclocking
// Must be called BEFORE changing system clock
// Based on Quake port approach
// FLASH_MAX_FREQ_MHZ is set via CMake -DFLASH_SPEED=xx (default 88)
#ifndef FLASH_MAX_FREQ_MHZ
#define FLASH_MAX_FREQ_MHZ 88
#endif

static void __no_inline_not_in_flash_func(set_flash_timings)(int cpu_mhz) {
    const int clock_hz = cpu_mhz * 1000000;
    const int max_flash_freq = FLASH_MAX_FREQ_MHZ * 1000000;

    int divisor = (clock_hz + max_flash_freq - (max_flash_freq >> 4) - 1) / max_flash_freq;
    if (divisor == 1 && clock_hz >= 166000000) {
        divisor = 2;
    }

    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000 && clock_hz >= 166000000) {
        rxdelay += 1;
    }

    qmi_hw->m[0].timing = 0x60007000 |
                        rxdelay << QMI_M0_TIMING_RXDELAY_LSB |
                        divisor << QMI_M0_TIMING_CLKDIV_LSB;
}

// Selected GRP filename (global for game access)
static char selected_grp[64] = "DUKE3D.GRP";

const char *get_selected_grp(void) {
    return selected_grp;
}

int main() {
    // Overclock support: For speeds > 252 MHz, increase voltage first
    // Based on Quake port initialization sequence
#if CPU_CLOCK_MHZ > 252
    vreg_disable_voltage_limit();
    vreg_set_voltage(CPU_VOLTAGE);
    set_flash_timings(CPU_CLOCK_MHZ);  // Set flash timings BEFORE clock change
    sleep_ms(100);  // Wait for voltage and timings to stabilize
#endif

    // Set system clock (252 MHz for HDMI, or overclocked)
    // 640x480@60Hz pixel clock is ~25.2MHz, PIO DVI needs 10x = ~252MHz
    // 378 MHz / 15 = 25.2 MHz (also works for HDMI)
    // 504 MHz / 20 = 25.2 MHz (also works for HDMI)
    if (!set_sys_clock_khz(CPU_CLOCK_MHZ * 1000, false)) {
        // Fallback to safe clock if requested speed fails
        set_sys_clock_khz(252 * 1000, true);
    }

    stdio_init_all();

    // Brief startup delay for USB serial connection
    for (int i = 0; i < 3; i++) {
        sleep_ms(500);
    }

    printf("\n=== FRANK Duke3D ===\n");
    printf("System Clock: %lu Hz\n", clock_get_hz(clk_sys));

    // Initialize PSRAM (required for game data)
    uint psram_pin = get_psram_pin();
    psram_init(psram_pin);

    // Initialize PSRAM linker sections (copy .psram_data, zero .psram_bss)
    psram_sections_init();

    // Allocate game data arrays in PSRAM
    psram_data_init();

    // Initialize welcome screen (sets up HDMI, PS/2)
    welcome_init();

    // Main game loop - allows returning to welcome screen
    while (1) {
        // Clear any previous return request
        welcome_clear_return();

        // Show welcome screen and get GRP selection
        const char *grp_file = welcome_show();

        if (grp_file) {
            // Copy selected GRP filename
            strncpy(selected_grp, grp_file, sizeof(selected_grp) - 1);
            selected_grp[sizeof(selected_grp) - 1] = '\0';

            printf("\nStarting Duke Nukem 3D with %s...\n", selected_grp);

            // Set game directory
            set_game_dir("/duke3d");

            // Launch Duke3D (GRP file is passed via get_selected_grp())
            char *argv[] = {"duke3d", NULL};
            main_duke3d(1, argv);

            // If we return here, game exited - loop back to welcome screen
            printf("\nGame exited, returning to welcome screen...\n");
            printf("Calling welcome_init()...\n");

            // Re-initialize welcome screen graphics
            welcome_init();
            printf("welcome_init() done, continuing to welcome_show()\n");
        } else {
            // No GRP files found - wait and retry
            printf("No GRP files found. Waiting...\n");
            sleep_ms(3000);
        }
    }

    return 0;
}
