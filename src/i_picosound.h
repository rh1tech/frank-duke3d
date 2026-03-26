/*
 * Duke3D Sound System for RP2350
 * Uses I2S audio via PIO + DMA ping-pong driver
 */

#ifndef __I_PICO_SOUND_H
#define __I_PICO_SOUND_H

#include "pico.h"
#include <stdbool.h>
#include <stdint.h>

// Lightweight audio buffer (replaces pico-extras audio_buffer_t)
typedef struct audio_buffer_data {
    uint8_t *bytes;
    uint32_t size;
} audio_buffer_data_t;

typedef struct audio_buffer {
    audio_buffer_data_t buf_storage;
    audio_buffer_data_t *buffer;
    uint32_t max_sample_count;
    uint32_t sample_count;
} audio_buffer_t;

// Audio sample rate - must match OPL2 native rate (3579545/72) when using
// EMU8950_NO_RATECONV, otherwise OPL oscillator frequencies and envelopes
// run at the wrong speed.
#ifndef PICO_SOUND_SAMPLE_FREQ
#define PICO_SOUND_SAMPLE_FREQ 49716
#endif

// Number of sound channels for sound effects
#ifndef NUM_SOUND_CHANNELS
#define NUM_SOUND_CHANNELS 8
#endif

// Game tick rate - Duke3D runs at ~30fps
#ifndef TICRATE
#define TICRATE 30
#endif

// Samples per audio buffer - one tick's worth of audio
#ifndef PICO_SOUND_BUFFER_SAMPLES
#define PICO_SOUND_BUFFER_SAMPLES ((PICO_SOUND_SAMPLE_FREQ + (TICRATE - 1)) / TICRATE)
#endif

// Enable low-pass filtering to reduce resampling artifacts
#ifndef SOUND_LOW_PASS
#define SOUND_LOW_PASS 1
#endif

//=============================================================================
// Sound System Interface
//=============================================================================

// Initialize the I2S audio system
// Returns true on success
bool I_PicoSound_Init(int numvoices, int mixrate);

// Shutdown the sound system
void I_PicoSound_Shutdown(void);

// Update sound - call once per game tick
// This mixes audio and sends buffers to I2S
void I_PicoSound_Update(void);

// Check if sound system is initialized
bool I_PicoSound_IsInitialized(void);

//=============================================================================
// Sound Playback Interface
//=============================================================================

// Play a VOC format sound
// Returns voice handle (>0) or 0 on failure
int I_PicoSound_PlayVOC(const uint8_t *data, uint32_t length,
                        int samplerate, int pitchoffset,
                        int vol, int left, int right,
                        int priority, uint32_t callbackval,
                        bool looping, uint32_t loopstart, uint32_t loopend);

// Play a WAV format sound
// Returns voice handle (>0) or 0 on failure
int I_PicoSound_PlayWAV(const uint8_t *data, uint32_t length,
                        int pitchoffset,
                        int vol, int left, int right,
                        int priority, uint32_t callbackval,
                        bool looping, uint32_t loopstart, uint32_t loopend);

// Play raw PCM data
// Returns voice handle (>0) or 0 on failure
int I_PicoSound_PlayRaw(const uint8_t *data, uint32_t length,
                        uint32_t samplerate, int pitchoffset,
                        int vol, int left, int right,
                        int priority, uint32_t callbackval,
                        bool looping, const uint8_t *loopstart, const uint8_t *loopend);

// Stop a sound by voice handle
int I_PicoSound_StopVoice(int handle);

// Stop all sounds
void I_PicoSound_StopAllVoices(void);

// Check if a voice is still playing
bool I_PicoSound_VoicePlaying(int handle);

// Get number of voices currently playing
int I_PicoSound_VoicesPlaying(void);

// Check if a voice slot is available at given priority
bool I_PicoSound_VoiceAvailable(int priority);

// Update pan/volume for a voice
void I_PicoSound_SetPan(int handle, int vol, int left, int right);

// Update pitch for a voice
void I_PicoSound_SetPitch(int handle, int pitchoffset);

// Update frequency for a voice
void I_PicoSound_SetFrequency(int handle, int frequency);

// Stop looping (let sound play to end)
void I_PicoSound_EndLooping(int handle);

// Set 3D pan (angle 0-255, distance 0-255)
void I_PicoSound_Pan3D(int handle, int angle, int distance);

//=============================================================================
// Volume Control
//=============================================================================

// Set master volume (0-255)
void I_PicoSound_SetVolume(int volume);

// Get master volume
int I_PicoSound_GetVolume(void);

// Set reverse stereo mode
void I_PicoSound_SetReverseStereo(bool reverse);

// Get reverse stereo mode
bool I_PicoSound_GetReverseStereo(void);

//=============================================================================
// Callback Support
//=============================================================================

// Set callback function for when a sound finishes
void I_PicoSound_SetCallback(void (*callback)(int32_t));

//=============================================================================
// Music Generator
//=============================================================================

// Set a function to generate music into audio buffers
void I_PicoSound_SetMusicGenerator(void (*generator)(audio_buffer_t *buffer));

#endif // __I_PICO_SOUND_H
