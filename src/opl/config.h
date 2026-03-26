// Duke3D OPL Music - Config header
// Adapted from murmdoom for Duke Nukem 3D RP2350 port

#ifndef __OPL_CONFIG_H__
#define __OPL_CONFIG_H__

// Use emu8950 OPL emulator
#define USE_EMU8950_OPL 1

// Disable unused features to save memory
#define EMU8950_NO_TIMER 1
#define EMU8950_NO_PERCUSSION_MODE 0
#define EMU8950_NO_RATECONV 1
#define EMU8950_NO_WAVE_TABLE_MAP 0
#define EMU8950_SLOT_RENDER 1

// For small builds
#define DOOM_SMALL 1
#define DOOM_TINY 1

// OPL timing
#define OPL_SECOND 1000000ULL

// Number of OPL voices
#define OPL_NUM_VOICES 9

#endif
