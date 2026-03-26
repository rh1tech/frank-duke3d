/*
 * Duke3D OPL Music System for RP2350
 * Uses emu8950 OPL emulator for FM synthesis
 * Parses standard MIDI files from SD card
 * Uses Duke3D native timbre bank format
 *
 * Based on murmdoom OPL music implementation by Graham Sanderson
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "pico.h"
#include "i_music.h"
#include "i_picosound.h"
#include "opl/emu8950.h"
#include "opl/midifile.h"
#include "../drivers/psram_allocator.h"
#include "../components/Engine/filesystem.h"  // For kopen4load, kread, etc.

// OPL configuration
#define OPL_SAMPLE_RATE PICO_SOUND_SAMPLE_FREQ
#define OPL_CLOCK       3579545     // OPL2 clock frequency
#define OPL_NUM_VOICES  9
#define OPL_SECOND      1000000ULL  // Microseconds per second

// From Duke3D _al_midi.h
#define NOTE_ON         0x2000      // Used to turn note on or toggle note
#define MAX_VELOCITY    0x7f
#define MAX_OCTAVE      7
#define MAX_NOTE        (MAX_OCTAVE * 12 + 11)

// Duke3D timbre format (13 bytes per instrument)
typedef struct {
    uint8_t SAVEK[2];    // Modulator/Carrier characteristics (reg 0x20)
    uint8_t Level[2];    // Modulator/Carrier output levels (reg 0x40)
    uint8_t Env1[2];     // Modulator/Carrier attack/decay (reg 0x60)
    uint8_t Env2[2];     // Modulator/Carrier sustain/release (reg 0x80)
    uint8_t Wave[2];     // Modulator/Carrier waveforms (reg 0xE0)
    uint8_t Feedback;    // Feedback/connection (reg 0xC0)
    int8_t Transpose;    // Note transpose offset
    int8_t Velocity;     // Velocity sensitivity
} timbre_t;

// OPL voice state (from Duke3D VOICE struct)
typedef struct {
    bool active;
    int channel;         // MIDI channel
    int key;             // MIDI note number (key)
    int velocity;        // Note velocity
    int timbre;          // Instrument/timbre number (-1 = none)
    int status;          // NOTE_ON or 0
    int pitchleft;       // Current pitch value
} opl_voice_t;

// MIDI channel state (from Duke3D CHANNEL struct)
typedef struct {
    int Timbre;          // Current program/instrument
    int volume;          // Channel volume (0-127)
    int pitchbend;       // Pitch bend
    int pan;             // Pan position (0-127)
    int KeyOffset;       // Key offset (transposition)
    int KeyDetune;       // Fine tuning (0-31)
} midi_channel_t;

// Module state
static OPL *opl_emu = NULL;
static midi_file_t *current_midi = NULL;
static midi_track_iter_t **track_iters = NULL;
static uint64_t *track_next_event_us = NULL;  // Next event time for each track
static unsigned int num_tracks = 0;
static unsigned int running_tracks = 0;
static bool music_initialized = false;
static bool music_playing = false;
static bool music_paused = false;
static bool music_looping = false;
static int music_volume = 153;  // ~60% volume (0-255 range)

// Timbre bank
static timbre_t timbre_bank[256];
static bool timbre_loaded = false;

// Voice allocation
static opl_voice_t voices[OPL_NUM_VOICES];
static midi_channel_t channels[16];

// Per-slot voice level and KSL storage (from Duke3D)
static int VoiceLevel[18];  // NumChipSlots = 18
static int VoiceKsl[18];

// Timing
static uint64_t current_time_us = 0;
static unsigned int us_per_beat = 500000;   // Default 120 BPM
static unsigned int ticks_per_beat = 480;

// Temp buffer for OPL output
static int32_t opl_temp_buffer[1024];

//=============================================================================
// Duke3D Lookup Tables (from al_midi.c)
//=============================================================================

// Octave pitch values (from _al_midi.h)
static const unsigned int OctavePitch[8] = {
    0x0000, 0x0400, 0x0800, 0x0C00,
    0x1000, 0x1400, 0x1800, 0x1C00
};

// Note pitch F-numbers (from al_midi.c - first row of NotePitch array, detune=0)
static const unsigned int NotePitch[12] = {
    0x157, 0x16b, 0x181, 0x198, 0x1b0, 0x1ca, 
    0x1e5, 0x202, 0x220, 0x241, 0x263, 0x287
};

// Slot numbers for each voice - modulator and carrier (from al_midi.c)
static const int slotVoice[OPL_NUM_VOICES][2] = {
    { 0, 3 },    // voice 0
    { 1, 4 },    // 1
    { 2, 5 },    // 2
    { 6, 9 },    // 3
    { 7, 10 },   // 4
    { 8, 11 },   // 5
    { 12, 15 },  // 6
    { 13, 16 },  // 7
    { 14, 17 },  // 8
};

// Offset of each slot within the chip registers (from al_midi.c)
static const int offsetSlot[18] = {
    0,  1,  2,  3,  4,  5,
    8,  9, 10, 11, 12, 13,
   16, 17, 18, 19, 20, 21
};

//=============================================================================
// OPL Register Helpers
//=============================================================================

static void OPL_Write(uint8_t reg, uint8_t value) {
    if (opl_emu) {
        OPL_writeReg(opl_emu, reg, value);
    }
}

/*
 * AL_SetVoiceTimbre - exact port from Duke3D al_midi.c
 * Programs the specified voice's timbre.
 */
static void AL_SetVoiceTimbre(int voice) {
    if (voice < 0 || voice >= OPL_NUM_VOICES) return;
    if (!timbre_loaded) return;

    int channel = voices[voice].channel;
    int patch;
    
    // Determine patch from channel (percussion uses key + 128)
    if (channel == 9) {
        patch = voices[voice].key + 128;
    } else {
        patch = channels[channel].Timbre;
    }
    
    // Skip if already using this timbre
    if (voices[voice].timbre == patch) {
        return;
    }
    
    voices[voice].timbre = patch;
    const timbre_t *timbre = &timbre_bank[patch];
    
    int slot = slotVoice[voice][0];  // Modulator slot
    int off = offsetSlot[slot];
    
    // Store level and KSL for volume calculations
    VoiceLevel[slot] = 63 - (timbre->Level[0] & 0x3F);
    VoiceKsl[slot] = timbre->Level[0] & 0xC0;
    
    // Turn off voice and clear frequency
    OPL_Write(0xA0 + voice, 0);
    OPL_Write(0xB0 + voice, 0);
    
    // Let voice clear the release
    OPL_Write(0x80 + off, 0xFF);
    
    // Set modulator registers
    OPL_Write(0x60 + off, timbre->Env1[0]);     // Attack/Decay
    OPL_Write(0x80 + off, timbre->Env2[0]);     // Sustain/Release
    OPL_Write(0x20 + off, timbre->SAVEK[0]);    // Characteristics
    OPL_Write(0xE0 + off, timbre->Wave[0]);     // Waveform
    OPL_Write(0x40 + off, timbre->Level[0]);    // Level
    
    // Set feedback/connection (for OPL2, just the feedback nibble)
    OPL_Write(0xC0 + voice, timbre->Feedback & 0x0F);
    
    // Now set carrier (slot 1)
    slot = slotVoice[voice][1];
    off = offsetSlot[slot];
    
    // Store level and KSL for carrier
    VoiceLevel[slot] = 63 - (timbre->Level[1] & 0x3F);
    VoiceKsl[slot] = timbre->Level[1] & 0xC0;
    
    // Set carrier to silent initially
    OPL_Write(0x40 + off, 63);
    
    // Let voice clear the release
    OPL_Write(0x80 + off, 0xFF);
    
    // Set carrier registers
    OPL_Write(0x60 + off, timbre->Env1[1]);     // Attack/Decay
    OPL_Write(0x80 + off, timbre->Env2[1]);     // Sustain/Release
    OPL_Write(0x20 + off, timbre->SAVEK[1]);    // Characteristics
    OPL_Write(0xE0 + off, timbre->Wave[1]);     // Waveform
}

/*
 * AL_SetVoiceVolume - exact port from Duke3D al_midi.c
 * Sets the volume of the specified voice.
 */
static void AL_SetVoiceVolume(int voice) {
    if (voice < 0 || voice >= OPL_NUM_VOICES) return;
    if (voices[voice].timbre < 0) return;
    
    int channel = voices[voice].channel;
    const timbre_t *timbre = &timbre_bank[voices[voice].timbre];
    
    // Add timbre velocity adjustment
    int velocity = voices[voice].velocity + timbre->Velocity;
    if (velocity > MAX_VELOCITY) velocity = MAX_VELOCITY;
    if (velocity < 0) velocity = 0;
    
    int slot = slotVoice[voice][1];  // Carrier slot
    int off = offsetSlot[slot];
    
    // Volume calculation from Duke3D:
    // t1 = VoiceLevel * (velocity + 0x80) * ChannelVolume >> 15
    unsigned int t1 = (unsigned int)VoiceLevel[slot];
    t1 *= (velocity + 0x80);
    t1 = (channels[channel].volume * t1) >> 15;
    
    // Apply music volume scaling (music_volume is 0-255)
    t1 = (t1 * music_volume) >> 8;
    
    // Convert to attenuation: volume XOR 63, then add KSL bits
    unsigned int volume = (t1 ^ 63) & 0x3F;
    volume |= (unsigned int)VoiceKsl[slot];
    
    OPL_Write(0x40 + off, volume);
    
    // If additive synthesis (connection bit = 1), also set modulator volume
    if (timbre->Feedback & 0x01) {
        slot = slotVoice[voice][0];  // Modulator slot
        off = offsetSlot[slot];
        
        unsigned int t2 = (unsigned int)VoiceLevel[slot];
        t2 *= (velocity + 0x80);
        t2 = (channels[channel].volume * t2) >> 15;
        t2 = (t2 * music_volume) >> 8;
        
        volume = (t2 ^ 63) & 0x3F;
        volume |= (unsigned int)VoiceKsl[slot];
        
        OPL_Write(0x40 + off, volume);
    }
}

/*
 * AL_SetVoicePitch - exact port from Duke3D al_midi.c
 * Programs the pitch of the specified voice.
 */
static void AL_SetVoicePitch(int voice) {
    if (voice < 0 || voice >= OPL_NUM_VOICES) return;
    
    int channel = voices[voice].channel;
    int note;
    int patch;
    
    // Calculate note from channel type
    if (channel == 9) {
        // Percussion - note comes from timbre's transpose field
        patch = voices[voice].key + 128;
        note = timbre_bank[patch].Transpose;
    } else {
        // Melodic - note is key + transpose
        patch = channels[channel].Timbre;
        note = voices[voice].key + timbre_bank[patch].Transpose;
    }
    
    // Apply key offset (Duke3D uses -12 as default)
    note += channels[channel].KeyOffset - 12;
    
    // Clamp note to valid range
    if (note > MAX_NOTE) note = MAX_NOTE;
    if (note < 0) note = 0;
    
    // Calculate octave and scale note
    int Octave = note / 12;
    int ScaleNote = note % 12;
    
    // Build pitch value: OctavePitch | NotePitch
    int pitch = OctavePitch[Octave] | NotePitch[ScaleNote];
    
    voices[voice].pitchleft = pitch;
    
    // Add key-on status
    pitch |= voices[voice].status;
    
    // Write to OPL registers (low byte to A0, high byte to B0)
    OPL_Write(0xA0 + voice, pitch & 0xFF);
    OPL_Write(0xB0 + voice, (pitch >> 8) & 0xFF);
}

/*
 * AL_NoteOn - based on Duke3D al_midi.c
 * Plays a note on the specified voice (voice already allocated)
 */
static void AL_NoteOn(int voice, int channel, int key, int velocity) {
    if (voice < 0 || voice >= OPL_NUM_VOICES) return;
    
    // Store voice state
    voices[voice].key = key;
    voices[voice].channel = channel;
    voices[voice].velocity = velocity;
    voices[voice].status = NOTE_ON;
    voices[voice].active = true;
    
    // Set timbre, volume, and pitch (in that order, like Duke3D)
    AL_SetVoiceTimbre(voice);
    AL_SetVoiceVolume(voice);
    AL_SetVoicePitch(voice);
}

/*
 * AL_NoteOff - based on Duke3D al_midi.c
 * Turns off a note on the specified voice
 */
static void AL_NoteOff(int voice) {
    if (voice < 0 || voice >= OPL_NUM_VOICES) return;
    if (!voices[voice].active) return;
    
    // Clear key-on status
    voices[voice].status = 0;
    
    // Re-write pitch with key-on cleared (allows natural release)
    int pitch = voices[voice].pitchleft;
    OPL_Write(0xA0 + voice, pitch & 0xFF);
    OPL_Write(0xB0 + voice, (pitch >> 8) & 0xFF);
    
    voices[voice].active = false;
}

//=============================================================================
// Voice Allocation
//=============================================================================

static int AllocateVoice(int channel, int key) {
    // Calculate the timbre for this note (for matching)
    int target_timbre;
    if (channel == 9) {
        target_timbre = key + 128;
    } else {
        target_timbre = channels[channel].Timbre;
    }
    
    // First, look for an inactive voice with the same timbre (avoids timbre switch click)
    for (int i = 0; i < OPL_NUM_VOICES; i++) {
        if (!voices[i].active && voices[i].timbre == target_timbre) {
            return i;
        }
    }
    
    // Second, look for any inactive voice
    for (int i = 0; i < OPL_NUM_VOICES; i++) {
        if (!voices[i].active) {
            return i;
        }
    }

    // All voices busy - need to steal one
    // Priority: steal same channel first, then percussion, then oldest
    int steal_voice = -1;
    
    // First try to steal from the same channel
    for (int i = 0; i < OPL_NUM_VOICES; i++) {
        if (voices[i].channel == channel) {
            steal_voice = i;
            break;
        }
    }
    
    // If that fails, try to steal a percussion voice (channel 9)
    if (steal_voice < 0) {
        for (int i = 0; i < OPL_NUM_VOICES; i++) {
            if (voices[i].channel == 9) {
                steal_voice = i;
                break;
            }
        }
    }
    
    // If still nothing, steal voice 0
    if (steal_voice < 0) {
        steal_voice = 0;
    }
    
    AL_NoteOff(steal_voice);
    return steal_voice;
}

static int FindVoice(int channel, int key) {
    for (int i = 0; i < OPL_NUM_VOICES; i++) {
        if (voices[i].active && voices[i].channel == channel && voices[i].key == key) {
            return i;
        }
    }
    return -1;
}

static void AllNotesOff(int channel) {
    for (int i = 0; i < OPL_NUM_VOICES; i++) {
        if (voices[i].active && voices[i].channel == channel) {
            AL_NoteOff(i);
        }
    }
}

//=============================================================================
// MIDI Event Processing
//=============================================================================

static void ProcessMIDIEvent(midi_event_t *event) {
    if (!event) return;

    switch (event->event_type) {
        case MIDI_EVENT_NOTE_OFF: {
            int ch = event->data.channel.channel;
            int note = event->data.channel.param1;
            int voice = FindVoice(ch, note);
            if (voice >= 0) {
                AL_NoteOff(voice);
            }
            break;
        }

        case MIDI_EVENT_NOTE_ON: {
            int ch = event->data.channel.channel;
            int note = event->data.channel.param1;
            int vel = event->data.channel.param2;
            
            if (vel == 0) {
                // Note on with velocity 0 = note off
                int voice = FindVoice(ch, note);
                if (voice >= 0) {
                    AL_NoteOff(voice);
                }
            } else {
                int voice = AllocateVoice(ch, note);
                AL_NoteOn(voice, ch, note, vel);
            }
            break;
        }

        case MIDI_EVENT_CONTROLLER: {
            int ch = event->data.channel.channel;
            int ctrl = event->data.channel.param1;
            int val = event->data.channel.param2;

            switch (ctrl) {
                case 7:  // Main volume
                    channels[ch].volume = val;
                    // Update all active voices on this channel
                    for (int i = 0; i < OPL_NUM_VOICES; i++) {
                        if (voices[i].active && voices[i].channel == ch) {
                            AL_SetVoiceVolume(i);
                        }
                    }
                    break;
                case 10: // Pan
                    channels[ch].pan = val;
                    break;
                case 123: // All notes off
                    AllNotesOff(ch);
                    break;
            }
            break;
        }

        case MIDI_EVENT_PROGRAM_CHANGE: {
            int ch = event->data.channel.channel;
            int prog = event->data.channel.param1;
            channels[ch].Timbre = prog;
            break;
        }

        case MIDI_EVENT_PITCH_BEND: {
            int ch = event->data.channel.channel;
            int bend = (event->data.channel.param2 << 7) | event->data.channel.param1;
            channels[ch].pitchbend = bend - 8192;
            // TODO: Apply pitch bend to active notes
            break;
        }

        case MIDI_EVENT_META: {
            if (event->data.meta.type == 0x51 && event->data.meta.length == 3) {
                // Set tempo
                uint8_t *data = event->data.meta.data;
                us_per_beat = (data[0] << 16) | (data[1] << 8) | data[2];
            }
            break;
        }

        default:
            break;
    }
}

//=============================================================================
// Music Generator Callback (called from audio mixer)
//=============================================================================

// Schedule next event for a track (calculates absolute time in microseconds)
static void ScheduleNextEvent(unsigned int track_num) {
    if (!track_iters || !track_iters[track_num]) return;
    
    unsigned int delta = MIDI_GetDeltaTime(track_iters[track_num]);
    uint64_t delta_us = ((uint64_t)delta * us_per_beat) / ticks_per_beat;
    track_next_event_us[track_num] = current_time_us + delta_us;
}

static void MusicGenerator(audio_buffer_t *buffer) {
    static uint32_t call_count = 0;
    call_count++;
    
    if (!buffer) return;
    
    unsigned int samples_to_fill = buffer->max_sample_count;
    int16_t *out = (int16_t *)buffer->buffer->bytes;
    
    // If music not playing, just clear the buffer and return
    if (!music_playing || music_paused || !opl_emu || !track_iters || !track_next_event_us) {
        memset(out, 0, samples_to_fill * 4);
        buffer->sample_count = samples_to_fill;
        return;
    }

    unsigned int filled = 0;
    int total_events_processed = 0;
    const int MAX_EVENTS_PER_BUFFER = 200;
    int loop_iterations = 0;
    const int MAX_LOOP_ITERATIONS = 2000;  // Safety limit

    while (filled < samples_to_fill) {
        // Safety check to prevent infinite loops
        if (++loop_iterations > MAX_LOOP_ITERATIONS) {
            break;
        }
        
        // Find earliest next event
        uint64_t next_event_time = UINT64_MAX;
        for (unsigned int t = 0; t < num_tracks; t++) {
            if (track_iters[t] && track_next_event_us[t] < next_event_time) {
                next_event_time = track_next_event_us[t];
            }
        }

        // Calculate samples until next event
        unsigned int samples_until_event;
        if (next_event_time == UINT64_MAX || next_event_time > current_time_us + 1000000) {
            samples_until_event = samples_to_fill - filled;
        } else if (next_event_time <= current_time_us) {
            samples_until_event = 0;
        } else {
            uint64_t us_until = next_event_time - current_time_us;
            samples_until_event = (us_until * OPL_SAMPLE_RATE) / OPL_SECOND;
            if (samples_until_event > samples_to_fill - filled) {
                samples_until_event = samples_to_fill - filled;
            }
        }

        // Generate OPL samples
        if (samples_until_event > 0) {
            unsigned int chunk = samples_until_event;
            if (chunk > 512) chunk = 512;

            OPL_calc_buffer_stereo(opl_emu, opl_temp_buffer, chunk);

            for (unsigned int i = 0; i < chunk; i++) {
                int32_t sample = opl_temp_buffer[i];
                int16_t left = (int16_t)(sample >> 16);
                int16_t right = (int16_t)(sample & 0xFFFF);
                // Amplify by 8x (matches Doom/Heretic OPL output level)
                out[(filled + i) * 2 + 0] = left << 3;
                out[(filled + i) * 2 + 1] = right << 3;
            }

            filled += chunk;
            current_time_us += (chunk * OPL_SECOND) / OPL_SAMPLE_RATE;
        } else if (total_events_processed < MAX_EVENTS_PER_BUFFER) {
            // Process one event from earliest track
            bool processed_any = false;
            for (unsigned int t = 0; t < num_tracks && !processed_any; t++) {
                if (!track_iters[t]) continue;
                if (track_next_event_us[t] > current_time_us) continue;

                midi_event_t *event;
                if (!MIDI_GetNextEvent(track_iters[t], &event)) {
                    running_tracks--;
                    track_iters[t] = NULL;
                    track_next_event_us[t] = UINT64_MAX;
                    processed_any = true;
                    continue;
                }

                ProcessMIDIEvent(event);
                total_events_processed++;
                processed_any = true;

                if (event->event_type == MIDI_EVENT_META && 
                    event->data.meta.type == 0x2F) {
                    running_tracks--;
                    track_iters[t] = NULL;
                    track_next_event_us[t] = UINT64_MAX;
                } else {
                    ScheduleNextEvent(t);
                }
            }
            
            if (!processed_any) {
                current_time_us += 1000;
            }
        } else {
            break;
        }

        if (running_tracks == 0) {
            if (music_looping && current_midi) {
                for (unsigned int t = 0; t < num_tracks; t++) {
                    if (track_iters[t]) {
                        MIDI_RestartIterator(track_iters[t]);
                    }
                }
                running_tracks = num_tracks;
                current_time_us = 0;
                for (unsigned int t = 0; t < num_tracks; t++) {
                    if (track_iters[t]) {
                        ScheduleNextEvent(t);
                    }
                }
            } else {
                music_playing = false;
                break;
            }
        }
    }

    // Fill remaining samples
    while (filled < samples_to_fill) {
        unsigned int chunk = samples_to_fill - filled;
        if (chunk > 512) chunk = 512;
        
        OPL_calc_buffer_stereo(opl_emu, opl_temp_buffer, chunk);
        
        for (unsigned int i = 0; i < chunk; i++) {
            int32_t sample = opl_temp_buffer[i];
            int16_t left = (int16_t)(sample >> 16);
            int16_t right = (int16_t)(sample & 0xFFFF);
            // Amplify by 10x
            int32_t amp_left = (int32_t)left * 10;
            int32_t amp_right = (int32_t)right * 10;
            if (amp_left > 32767) amp_left = 32767;
            if (amp_left < -32768) amp_left = -32768;
            if (amp_right > 32767) amp_right = 32767;
            if (amp_right < -32768) amp_right = -32768;
            out[(filled + i) * 2 + 0] = (int16_t)amp_left;
            out[(filled + i) * 2 + 1] = (int16_t)amp_right;
        }
        filled += chunk;
        current_time_us += (chunk * OPL_SECOND) / OPL_SAMPLE_RATE;
    }

    buffer->sample_count = samples_to_fill;
}

//=============================================================================
// Public API
//=============================================================================

bool I_Music_Init(void) {
    if (music_initialized) {
        return true;
    }

    // Initialize OPL emulator
    opl_emu = OPL_new(OPL_CLOCK, OPL_SAMPLE_RATE);
    if (!opl_emu) {
        return false;
    }

    // Initialize OPL registers
    OPL_reset(opl_emu);
    
    // Enable waveform select
    OPL_Write(0x01, 0x20);

    // Clear all voices
    for (int i = 0; i < OPL_NUM_VOICES; i++) {
        voices[i].active = false;
        OPL_Write(0xB0 + i, 0);  // Key off all voices
    }

    // Initialize channels
    for (int i = 0; i < 16; i++) {
        channels[i].Timbre = 0;
        channels[i].volume = 127;
        channels[i].pitchbend = 0;
        channels[i].pan = 64;
        channels[i].KeyOffset = 0;
        channels[i].KeyDetune = 0;
    }

    // NOTE: Don't register music generator here - do it when music starts
    // This avoids issues with the callback being called before music is loaded

    music_initialized = true;
    return true;
}

void I_Music_Shutdown(void) {
    if (!music_initialized) return;

    I_Music_Stop();

    if (opl_emu) {
        OPL_delete(opl_emu);
        opl_emu = NULL;
    }

    music_initialized = false;
}

bool I_Music_PlayMIDI(const char *filename, bool loop) {
    if (!music_initialized) {
        if (!I_Music_Init()) {
            return false;
        }
    }

    // Stop any current playback
    I_Music_Stop();

    // Reset temp PSRAM allocation
    psram_reset_temp();
    psram_set_temp_mode(1);

    // Load MIDI file from GRP archive using Duke3D's file functions
    int32_t fd = kopen4load(filename, 0);  // 0 = try filesystem first, then GRP
    if (fd < 0) {
        psram_set_temp_mode(0);
        return false;
    }

    int32_t fileSize = kfilelength(fd);
    if (fileSize <= 0) {
        kclose(fd);
        psram_set_temp_mode(0);
        return false;
    }

    // Read MIDI data into buffer
    uint8_t *midiBuffer = psram_malloc(fileSize);
    if (!midiBuffer) {
        kclose(fd);
        psram_set_temp_mode(0);
        return false;
    }

    int32_t bytesRead = kread(fd, midiBuffer, fileSize);
    kclose(fd);

    if (bytesRead != fileSize) {
        psram_free(midiBuffer);
        psram_set_temp_mode(0);
        return false;
    }

    // Write to temp file on SD card so MIDI loader can read it
    const char *tempPath = "/duke3d/temp.mid";
    
    // Remove old temp file first to ensure clean write
    remove(tempPath);
    
    FILE *tempFile = fopen(tempPath, "wb");
    if (!tempFile) {
        psram_free(midiBuffer);
        psram_set_temp_mode(0);
        return false;
    }

    size_t written = fwrite(midiBuffer, 1, fileSize, tempFile);
    fflush(tempFile);
    fclose(tempFile);
    
    psram_free(midiBuffer);

    if (written != (size_t)fileSize) {
        psram_set_temp_mode(0);
        return false;
    }

    // Now load from the temp file
    current_midi = MIDI_LoadFile((char *)tempPath);
    
    psram_set_temp_mode(0);

    if (!current_midi) {
        return false;
    }

    // Get MIDI info
    num_tracks = MIDI_NumTracks(current_midi);
    ticks_per_beat = MIDI_GetFileTimeDivision(current_midi);
    us_per_beat = 500000;  // Default 120 BPM

    // Allocate track iterators
    track_iters = psram_malloc(num_tracks * sizeof(midi_track_iter_t *));
    if (!track_iters) {
        MIDI_FreeFile(current_midi);
        current_midi = NULL;
        return false;
    }

    // Allocate track timing array
    track_next_event_us = psram_malloc(num_tracks * sizeof(uint64_t));
    if (!track_next_event_us) {
        psram_free(track_iters);
        track_iters = NULL;
        MIDI_FreeFile(current_midi);
        current_midi = NULL;
        return false;
    }

    // Initialize track iterators and schedule first events
    for (unsigned int i = 0; i < num_tracks; i++) {
        track_iters[i] = MIDI_IterateTrack(current_midi, i);
        track_next_event_us[i] = 0;  // First event at time 0
    }
    
    // Schedule first events for each track
    current_time_us = 0;
    for (unsigned int i = 0; i < num_tracks; i++) {
        ScheduleNextEvent(i);
    }
    
    running_tracks = num_tracks;

    // Reset channels to defaults
    for (int i = 0; i < 16; i++) {
        channels[i].Timbre = 0;
        channels[i].volume = 127;
        channels[i].pitchbend = 0;
        channels[i].pan = 64;
        channels[i].KeyOffset = 0;
        channels[i].KeyDetune = 0;
    }

    // Reset OPL chip and voices for clean start
    OPL_reset(opl_emu);
    OPL_Write(0x01, 0x20);  // Enable waveform select
    for (int i = 0; i < OPL_NUM_VOICES; i++) {
        voices[i].active = false;
        voices[i].channel = 0;
        voices[i].key = 0;
        voices[i].velocity = 0;
        voices[i].timbre = -1;
        voices[i].status = 0;
        voices[i].pitchleft = 0;
        OPL_Write(0xB0 + i, 0);  // Key off
    }

    // Start playback
    music_looping = loop;
    music_paused = false;
    music_playing = true;

    // Register music generator callback now that music is ready
    if (I_PicoSound_IsInitialized()) {
        I_PicoSound_SetMusicGenerator(MusicGenerator);
    }

    return true;
}

void I_Music_Stop(void) {
    music_playing = false;
    music_paused = false;

    // Unregister music generator callback
    if (I_PicoSound_IsInitialized()) {
        I_PicoSound_SetMusicGenerator(NULL);
    }

    // Stop all notes
    for (int i = 0; i < OPL_NUM_VOICES; i++) {
        if (voices[i].active) {
            AL_NoteOff(i);
        }
    }

    // Free track iterators
    if (track_iters) {
        for (unsigned int i = 0; i < num_tracks; i++) {
            if (track_iters[i]) {
                MIDI_FreeIterator(track_iters[i]);
            }
        }
        psram_free(track_iters);
        track_iters = NULL;
    }

    // Free track timing array
    if (track_next_event_us) {
        psram_free(track_next_event_us);
        track_next_event_us = NULL;
    }

    // Free MIDI file
    if (current_midi) {
        MIDI_FreeFile(current_midi);
        current_midi = NULL;
    }

    num_tracks = 0;
    running_tracks = 0;
    
    // Reset temp PSRAM
    psram_reset_temp();
}

void I_Music_Pause(void) {
    if (!music_playing) return;
    music_paused = true;

    // Stop all active notes
    for (int i = 0; i < OPL_NUM_VOICES; i++) {
        if (voices[i].active) {
            // Just key off without clearing state
            OPL_Write(0xB0 + i, 0);
        }
    }
}

void I_Music_Resume(void) {
    music_paused = false;
}

bool I_Music_IsPlaying(void) {
    return music_playing && !music_paused;
}

void I_Music_SetVolume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 255) volume = 255;
    music_volume = volume;  // Keep full 0-255 range for more headroom
}

int I_Music_GetVolume(void) {
    return music_volume;
}

void I_Music_RegisterTimbreBank(const uint8_t *timbres) {
    if (!timbres) return;

    // Copy timbre data (256 instruments × 13 bytes)
    for (int i = 0; i < 256; i++) {
        timbre_bank[i].SAVEK[0] = *timbres++;
        timbre_bank[i].SAVEK[1] = *timbres++;
        timbre_bank[i].Level[0] = *timbres++;
        timbre_bank[i].Level[1] = *timbres++;
        timbre_bank[i].Env1[0] = *timbres++;
        timbre_bank[i].Env1[1] = *timbres++;
        timbre_bank[i].Env2[0] = *timbres++;
        timbre_bank[i].Env2[1] = *timbres++;
        timbre_bank[i].Wave[0] = *timbres++;
        timbre_bank[i].Wave[1] = *timbres++;
        timbre_bank[i].Feedback = *timbres++;
        timbre_bank[i].Transpose = (int8_t)*timbres++;
        timbre_bank[i].Velocity = (int8_t)*timbres++;
    }

    timbre_loaded = true;
}
