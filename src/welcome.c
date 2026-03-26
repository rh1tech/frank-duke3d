/*
 * Welcome Screen for Duke Nukem 3D - RP2350 Port
 * Demoscene-style plasma effect with GRP file selection
 */
#include "welcome.h"
#include "HDMI.h"
#include "psram_allocator.h"
#include "pico/stdlib.h"
#include "ff.h"
#include "ps2kbd_wrapper.h"
#include "usbhid_wrapper.h"
#include "SDL_video.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

// Version injected by CMake
#ifndef FRANKDUKE_VERSION
#define FRANKDUKE_VERSION "?"
#endif

// Board variant
#ifndef DBOARD_VARIANT
#if defined(BOARD_M2)
#define DBOARD_VARIANT "M2"
#else
#define DBOARD_VARIANT "M1"
#endif
#endif

//=============================================================================
// Framebuffer (shared with SDL_video_rp2350.c)
//=============================================================================

// External framebuffer from SDL_video_rp2350.c
extern uint8_t FRAME_BUF[];

static bool return_to_welcome = false;

//=============================================================================
// 5x7 Bitmap Font (from murmdoom)
//=============================================================================

static const uint8_t *glyph_5x7(char ch) {
    static const uint8_t glyph_space[7] = {0, 0, 0, 0, 0, 0, 0};
    static const uint8_t glyph_dot[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
    static const uint8_t glyph_comma[7] = {0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x08};
    static const uint8_t glyph_colon[7] = {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00};
    static const uint8_t glyph_hyphen[7] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
    static const uint8_t glyph_lparen[7] = {0x04, 0x08, 0x08, 0x08, 0x08, 0x08, 0x04};
    static const uint8_t glyph_rparen[7] = {0x04, 0x02, 0x02, 0x02, 0x02, 0x02, 0x04};
    static const uint8_t glyph_slash[7] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00};
    static const uint8_t glyph_underscore[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F};

    static const uint8_t glyph_0[7] = {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
    static const uint8_t glyph_1[7] = {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
    static const uint8_t glyph_2[7] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
    static const uint8_t glyph_3[7] = {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
    static const uint8_t glyph_4[7] = {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
    static const uint8_t glyph_5[7] = {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};
    static const uint8_t glyph_6[7] = {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E};
    static const uint8_t glyph_7[7] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
    static const uint8_t glyph_8[7] = {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
    static const uint8_t glyph_9[7] = {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E};

    static const uint8_t glyph_a[7] = {0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F};
    static const uint8_t glyph_b[7] = {0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x1E};
    static const uint8_t glyph_c[7] = {0x00, 0x00, 0x0E, 0x11, 0x10, 0x11, 0x0E};
    static const uint8_t glyph_d[7] = {0x01, 0x01, 0x0D, 0x13, 0x11, 0x13, 0x0D};
    static const uint8_t glyph_e[7] = {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0F};
    static const uint8_t glyph_f[7] = {0x06, 0x08, 0x1E, 0x08, 0x08, 0x08, 0x08};
    static const uint8_t glyph_g[7] = {0x00, 0x00, 0x0F, 0x11, 0x0F, 0x01, 0x0E};
    static const uint8_t glyph_h[7] = {0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x11};
    static const uint8_t glyph_i[7] = {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E};
    static const uint8_t glyph_j[7] = {0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0C};
    static const uint8_t glyph_k[7] = {0x10, 0x10, 0x11, 0x12, 0x1C, 0x12, 0x11};
    static const uint8_t glyph_l[7] = {0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x06};
    static const uint8_t glyph_m[7] = {0x00, 0x00, 0x1A, 0x15, 0x15, 0x15, 0x15};
    static const uint8_t glyph_n[7] = {0x00, 0x00, 0x1E, 0x11, 0x11, 0x11, 0x11};
    static const uint8_t glyph_o[7] = {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E};
    static const uint8_t glyph_p[7] = {0x00, 0x00, 0x1E, 0x11, 0x1E, 0x10, 0x10};
    static const uint8_t glyph_q[7] = {0x00, 0x00, 0x0D, 0x13, 0x13, 0x0D, 0x01};
    static const uint8_t glyph_r[7] = {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10};
    static const uint8_t glyph_s[7] = {0x00, 0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E};
    static const uint8_t glyph_t[7] = {0x04, 0x04, 0x1F, 0x04, 0x04, 0x04, 0x03};
    static const uint8_t glyph_u[7] = {0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0D};
    static const uint8_t glyph_v[7] = {0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04};
    static const uint8_t glyph_w[7] = {0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A};
    static const uint8_t glyph_x[7] = {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11};
    static const uint8_t glyph_y[7] = {0x00, 0x00, 0x11, 0x11, 0x0F, 0x01, 0x0E};
    static const uint8_t glyph_z[7] = {0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F};

    static const uint8_t glyph_A[7] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    static const uint8_t glyph_B[7] = {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
    static const uint8_t glyph_C[7] = {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
    static const uint8_t glyph_D[7] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
    static const uint8_t glyph_E[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
    static const uint8_t glyph_F[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
    static const uint8_t glyph_G[7] = {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E};
    static const uint8_t glyph_H[7] = {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    static const uint8_t glyph_I[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F};
    static const uint8_t glyph_J[7] = {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C};
    static const uint8_t glyph_K[7] = {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
    static const uint8_t glyph_L[7] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
    static const uint8_t glyph_M[7] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
    static const uint8_t glyph_N[7] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
    static const uint8_t glyph_O[7] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    static const uint8_t glyph_P[7] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
    static const uint8_t glyph_Q[7] = {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
    static const uint8_t glyph_R[7] = {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
    static const uint8_t glyph_S[7] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
    static const uint8_t glyph_T[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    static const uint8_t glyph_U[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    static const uint8_t glyph_V[7] = {0x11, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04};
    static const uint8_t glyph_W[7] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A};
    static const uint8_t glyph_X[7] = {0x11, 0x0A, 0x04, 0x04, 0x04, 0x0A, 0x11};
    static const uint8_t glyph_Y[7] = {0x11, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04};
    static const uint8_t glyph_Z[7] = {0x1F, 0x02, 0x04, 0x08, 0x10, 0x10, 0x1F};

    int c = (unsigned char)ch;
    switch (c) {
        case ' ': return glyph_space;
        case '.': return glyph_dot;
        case ',': return glyph_comma;
        case ':': return glyph_colon;
        case '-': return glyph_hyphen;
        case '(': return glyph_lparen;
        case ')': return glyph_rparen;
        case '/': return glyph_slash;
        case '_': return glyph_underscore;

        case '0': return glyph_0;
        case '1': return glyph_1;
        case '2': return glyph_2;
        case '3': return glyph_3;
        case '4': return glyph_4;
        case '5': return glyph_5;
        case '6': return glyph_6;
        case '7': return glyph_7;
        case '8': return glyph_8;
        case '9': return glyph_9;

        case 'a': return glyph_a;
        case 'b': return glyph_b;
        case 'c': return glyph_c;
        case 'd': return glyph_d;
        case 'e': return glyph_e;
        case 'f': return glyph_f;
        case 'g': return glyph_g;
        case 'h': return glyph_h;
        case 'i': return glyph_i;
        case 'j': return glyph_j;
        case 'k': return glyph_k;
        case 'l': return glyph_l;
        case 'm': return glyph_m;
        case 'n': return glyph_n;
        case 'o': return glyph_o;
        case 'p': return glyph_p;
        case 'q': return glyph_q;
        case 'r': return glyph_r;
        case 's': return glyph_s;
        case 't': return glyph_t;
        case 'u': return glyph_u;
        case 'v': return glyph_v;
        case 'w': return glyph_w;
        case 'x': return glyph_x;
        case 'y': return glyph_y;
        case 'z': return glyph_z;

        case 'A': return glyph_A;
        case 'B': return glyph_B;
        case 'C': return glyph_C;
        case 'D': return glyph_D;
        case 'E': return glyph_E;
        case 'F': return glyph_F;
        case 'G': return glyph_G;
        case 'H': return glyph_H;
        case 'I': return glyph_I;
        case 'J': return glyph_J;
        case 'K': return glyph_K;
        case 'L': return glyph_L;
        case 'M': return glyph_M;
        case 'N': return glyph_N;
        case 'O': return glyph_O;
        case 'P': return glyph_P;
        case 'Q': return glyph_Q;
        case 'R': return glyph_R;
        case 'S': return glyph_S;
        case 'T': return glyph_T;
        case 'U': return glyph_U;
        case 'V': return glyph_V;
        case 'W': return glyph_W;
        case 'X': return glyph_X;
        case 'Y': return glyph_Y;
        case 'Z': return glyph_Z;

        default: return glyph_space;
    }
}

static void draw_char_5x7(int x, int y, char ch, uint8_t color) {
    const uint8_t *rows = glyph_5x7(ch);
    for (int row = 0; row < 7; ++row) {
        int yy = y + row;
        if (yy < 0 || yy >= SCREEN_HEIGHT) continue;
        uint8_t bits = rows[row];
        for (int col = 0; col < 5; ++col) {
            int xx = x + col;
            if (xx < 0 || xx >= SCREEN_WIDTH) continue;
            if (bits & (1u << (4 - col))) {
                FRAME_BUF[yy * SCREEN_WIDTH + xx] = color;
            }
        }
    }
}

static void draw_text_5x7(int x, int y, const char *text, uint8_t color) {
    const int advance = 6;
    for (const char *p = text; *p; ++p) {
        draw_char_5x7(x, y, *p, color);
        x += advance;
    }
}

static int text_width_5x7(const char *text) {
    int len = 0;
    for (const char *p = text; *p; ++p) len++;
    return len * 6;
}

static void fill_rect(int x, int y, int w, int h, uint8_t color) {
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_WIDTH) w = SCREEN_WIDTH - x;
    if (y + h > SCREEN_HEIGHT) h = SCREEN_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    for (int yy = y; yy < y + h; ++yy) {
        memset(&FRAME_BUF[yy * SCREEN_WIDTH + x], color, (size_t)w);
    }
}

//=============================================================================
// Sine Table for Plasma Effect
//=============================================================================

#define SIN_TABLE_SIZE 256
static int8_t sin_table[SIN_TABLE_SIZE];

static void init_sin_table(void) {
    for (int i = 0; i < SIN_TABLE_SIZE; i++) {
        sin_table[i] = (int8_t)(127.0f * sinf(i * 2.0f * 3.14159f / SIN_TABLE_SIZE));
    }
}

//=============================================================================
// Demoscene Plasma Effect
//=============================================================================

static void draw_plasma_background(uint32_t t_ms, int panel_x, int panel_y, int panel_w, int panel_h) {
    const int t = (int)(t_ms / 20);  // Animation speed

    int panel_x2 = panel_x + panel_w;
    int panel_y2 = panel_y + panel_h;

    // Clamp panel bounds
    if (panel_x < 0) panel_x = 0;
    if (panel_y < 0) panel_y = 0;
    if (panel_x2 > SCREEN_WIDTH) panel_x2 = SCREEN_WIDTH;
    if (panel_y2 > SCREEN_HEIGHT) panel_y2 = SCREEN_HEIGHT;

    for (int y = 0; y < SCREEN_HEIGHT; ++y) {
        // Skip panel interior
        if (y >= panel_y && y < panel_y2) {
            // Only draw left and right borders
            for (int x = 0; x < panel_x; ++x) {
                // Classic plasma: combine multiple sine waves
                int v1 = sin_table[(x + t) & 0xFF];
                int v2 = sin_table[(y + t * 2) & 0xFF];
                int v3 = sin_table[((x + y + t) >> 1) & 0xFF];
                int v4 = sin_table[((x * 2 - y + t * 3) >> 2) & 0xFF];

                int plasma = (v1 + v2 + v3 + v4) / 4 + 128;
                uint8_t color = 2 + ((plasma * 15) >> 8);  // Map to palette 2-17
                FRAME_BUF[y * SCREEN_WIDTH + x] = color;
            }
            for (int x = panel_x2; x < SCREEN_WIDTH; ++x) {
                int v1 = sin_table[(x + t) & 0xFF];
                int v2 = sin_table[(y + t * 2) & 0xFF];
                int v3 = sin_table[((x + y + t) >> 1) & 0xFF];
                int v4 = sin_table[((x * 2 - y + t * 3) >> 2) & 0xFF];

                int plasma = (v1 + v2 + v3 + v4) / 4 + 128;
                uint8_t color = 2 + ((plasma * 15) >> 8);
                FRAME_BUF[y * SCREEN_WIDTH + x] = color;
            }
        } else {
            // Full row outside panel
            for (int x = 0; x < SCREEN_WIDTH; ++x) {
                int v1 = sin_table[(x + t) & 0xFF];
                int v2 = sin_table[(y + t * 2) & 0xFF];
                int v3 = sin_table[((x + y + t) >> 1) & 0xFF];
                int v4 = sin_table[((x * 2 - y + t * 3) >> 2) & 0xFF];

                int plasma = (v1 + v2 + v3 + v4) / 4 + 128;
                uint8_t color = 2 + ((plasma * 15) >> 8);
                FRAME_BUF[y * SCREEN_WIDTH + x] = color;
            }
        }
    }
}

//=============================================================================
// GRP File Scanning
//=============================================================================

// Compatible Duke3D GRP files (uppercase only - FAT is case-insensitive)
static const grp_entry_t duke_grps[] = {
    {"DUKE3D.GRP", "Duke Nukem 3D v1.5 Atomic"},
    {"DUKESW.GRP", "Duke Nukem 3D Shareware"},
    {"DUKEDC.GRP", "Duke It Out In D.C."},
    {"VACATION.GRP", "Duke Caribbean"},
    {"NWINTER.GRP", "Duke Nuclear Winter"},
};

#define MAX_GRP_FILES 10
static const grp_entry_t *available_grps[MAX_GRP_FILES];
static int available_count = 0;

static void scan_grp_files(void) {
    available_count = 0;

    printf("Scanning for GRP files in /duke3d/...\n");

    for (size_t i = 0; i < sizeof(duke_grps) / sizeof(duke_grps[0]) && available_count < MAX_GRP_FILES; ++i) {
        char path[64];
        snprintf(path, sizeof(path), "/duke3d/%s", duke_grps[i].filename);

        FILINFO info;
        if (f_stat(path, &info) == FR_OK) {
            printf("  Found: %s (%lu bytes)\n", duke_grps[i].filename, (unsigned long)info.fsize);
            available_grps[available_count++] = &duke_grps[i];
        }
    }

    if (available_count == 0) {
        printf("  No compatible GRP files found!\n");
        printf("  Place DUKE3D.GRP in /duke3d/ folder on SD card.\n");
    }
}

//=============================================================================
// Menu Rendering
//=============================================================================

static void render_menu(int selected, int menu_x, int menu_y, int menu_w, int line_h) {
    // Clear menu area
    fill_rect(menu_x - 2, menu_y - 2, menu_w + 4, available_count * line_h + 4, 0);

    if (available_count == 0) {
        draw_text_5x7(menu_x, menu_y, "No GRP files found!", 1);
        draw_text_5x7(menu_x, menu_y + 10, "Place DUKE3D.GRP in", 1);
        draw_text_5x7(menu_x, menu_y + 20, "/duke3d/ on SD card", 1);
        return;
    }

    for (int i = 0; i < available_count; ++i) {
        const int y = menu_y + i * line_h;

        if (i == selected) {
            // Highlighted: white background, black text
            fill_rect(menu_x - 2, y - 1, menu_w + 4, 9, 1);
            draw_text_5x7(menu_x, y, available_grps[i]->label, 0);
        } else {
            draw_text_5x7(menu_x, y, available_grps[i]->label, 1);
        }
    }
}

//=============================================================================
// Keyboard Input
//=============================================================================

// Duke3D scancodes
#define sc_Return    0x1c
#define sc_Escape    0x01
#define sc_UpArrow   0x5a
#define sc_DownArrow 0x6a
#define sc_W         0x11
#define sc_S         0x1f

static bool get_key(int *pressed, unsigned char *key) {
    // Poll PS/2 keyboard
    ps2kbd_tick();
    if (ps2kbd_get_key(pressed, key)) {
        return true;
    }

    // Poll USB HID keyboard (if enabled)
    if (usbhid_wrapper_get_key(pressed, key)) {
        return true;
    }

    return false;
}

//=============================================================================
// Public Interface
//=============================================================================

void welcome_init(void) {
    static bool first_init = true;
    static FATFS fs;

    if (first_init) {
        // Mount SD Card (only on first init)
        printf("Mounting SD card...\n");
        FRESULT fr = f_mount(&fs, "", 1);
        if (fr != FR_OK) {
            printf("Failed to mount SD card: %d\n", fr);
        } else {
            printf("SD card mounted successfully\n");
        }

        // Initialize PS/2 keyboard
        ps2kbd_init();

        // Initialize USB HID (if enabled)
        usbhid_wrapper_init();

        first_init = false;
    } else {
        // Reset SDL video state when returning from game
        printf("welcome_init: Resetting SDL video state...\n");
        SDL_ResetVideoState();
    }

    // Initialize HDMI (skipped if already done)
    printf("welcome_init: Setting up graphics %dx%d\n", SCREEN_WIDTH, SCREEN_HEIGHT);
    graphics_init(g_out_HDMI);
    // Always reset resolution to 320x240 for welcome screen
    graphics_set_res(SCREEN_WIDTH, SCREEN_HEIGHT);
    graphics_set_buffer(FRAME_BUF);
    printf("welcome_init: Graphics setup done\n");

    // Initialize sine table for plasma
    init_sin_table();

    // Set up palette
    // 0 = black, 1 = white
    graphics_set_palette(0, 0x000000);
    graphics_set_palette(1, 0xFFFFFF);

    // Plasma palette (2-17): Duke3D orange/brown colors
    static const uint32_t plasma_pal[16] = {
        0x100800, 0x180C00, 0x201000, 0x281400,
        0x301800, 0x381C00, 0x402000, 0x482400,
        0x502800, 0x582C00, 0x603000, 0x683400,
        0x703800, 0x783C00, 0x804000, 0x884400,
    };
    for (int i = 0; i < 16; ++i) {
        graphics_set_palette(2 + i, plasma_pal[i]);
    }

    // Title highlight color (bright orange)
    graphics_set_palette(18, 0xFF6600);

    // Clear screen
    memset(FRAME_BUF, 0, SCREEN_WIDTH * SCREEN_HEIGHT);

    printf("Welcome screen initialized\n");
}

const char *welcome_show(void) {
    // Scan for GRP files
    scan_grp_files();

    // Panel dimensions
    const int panel_x = 24;
    const int panel_y = 24;
    const int panel_w = SCREEN_WIDTH - 48;
    const int panel_h = SCREEN_HEIGHT - 48;

    // Menu position
    const int menu_x = panel_x + 8;
    const int menu_y = panel_y + 50;
    const int line_h = 10;
    const int menu_w = panel_w - 16;

    int selected = 0;
    int prev_selected = -1;

    // Build title strings
    char title_right[64];
    snprintf(title_right, sizeof(title_right), " v%s", FRANKDUKE_VERSION);
    const char *title_left = "FRANK Duke3D";

    int title_left_w = text_width_5x7(title_left);
    int title_right_w = text_width_5x7(title_right);
    int title_w = title_left_w + title_right_w;
    int title_x = (SCREEN_WIDTH - title_w) / 2;
    int title_y = panel_y + 10;

    // Status lines
    char status1[64];
    char status2[64];
    char status3[64];
    char status4[64];
    snprintf(status1, sizeof(status1), "Up/Down: select, Enter: start");
    snprintf(status2, sizeof(status2), "Board: %s, %d/%d MHz", DBOARD_VARIANT, CPU_CLOCK_MHZ, PSRAM_MAX_FREQ_MHZ);
    snprintf(status3, sizeof(status3), "Port by Mikhail Matveev");
    snprintf(status4, sizeof(status4), "https://github.com/rh1tech/frank-duke3d");

    // Draw static panel content
    draw_plasma_background(0, panel_x, panel_y, panel_w, panel_h);
    fill_rect(panel_x, panel_y, panel_w, panel_h, 0);

    // Title with highlight
    fill_rect(title_x - 2, title_y - 2, title_left_w + 4, 11, 18);
    draw_text_5x7(title_x, title_y, title_left, 0);
    draw_text_5x7(title_x + title_left_w, title_y, title_right, 1);

    // "Select GRP:" label
    draw_text_5x7(menu_x, menu_y - 14, "Select GRP file:", 1);

    // Status text at bottom
    int bottom_y = panel_y + panel_h - 48;
    draw_text_5x7(menu_x, bottom_y, status1, 1);
    draw_text_5x7(menu_x, bottom_y + 10, status2, 1);
    draw_text_5x7(menu_x, bottom_y + 20, status3, 1);
    draw_text_5x7(menu_x, bottom_y + 30, status4, 1);

    // Main selection loop
    while (true) {
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());

        // Animate plasma background (only border area)
        draw_plasma_background(now_ms, panel_x, panel_y, panel_w, panel_h);

        // Update menu if selection changed
        if (prev_selected != selected) {
            render_menu(selected, menu_x, menu_y, menu_w, line_h);
            prev_selected = selected;
        }

        // Handle keyboard input
        int pressed = 0;
        unsigned char key = 0;
        while (get_key(&pressed, &key)) {
            if (!pressed) continue;

            if (key == sc_Return && available_count > 0) {
                // Show loading message
                fill_rect(panel_x, panel_y, panel_w, panel_h, 0);
                char msg[64];
                snprintf(msg, sizeof(msg), "Loading %s...", available_grps[selected]->filename);
                int msg_w = text_width_5x7(msg);
                int msg_x = (SCREEN_WIDTH - msg_w) / 2;
                int msg_y = SCREEN_HEIGHT / 2 - 4;
                draw_text_5x7(msg_x, msg_y, msg, 1);

                return available_grps[selected]->filename;
            }

            if (available_count <= 0) continue;

            if (key == sc_UpArrow || key == sc_W) {
                selected = (selected - 1 + available_count) % available_count;
            } else if (key == sc_DownArrow || key == sc_S) {
                selected = (selected + 1) % available_count;
            }
        }

        sleep_ms(33);  // ~30 FPS
    }

    return NULL;
}

void welcome_request_return(void) {
    return_to_welcome = true;
}

bool welcome_should_return(void) {
    return return_to_welcome;
}

void welcome_clear_return(void) {
    return_to_welcome = false;
}
