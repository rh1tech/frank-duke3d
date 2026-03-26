/*
 * Duke3D Sound System for RP2350
 * Uses I2S audio via PIO + DMA ping-pong driver
 *
 * Copyright (C) 2024
 * Portions from murmdoom (C) 2021-2022 Graham Sanderson
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "i_picosound.h"
#include "audio.h"
#include "board_config.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"

#ifndef INT16_MAX
#define INT16_MAX 32767
#endif
#ifndef INT16_MIN
#define INT16_MIN (-32768)
#endif

//=============================================================================
// Data Types
//=============================================================================

// Small decompressed buffer size - matches murmdoom's approach
// Buffer is refilled during mixing when exhausted
#define VOICE_BUFFER_SAMPLES 256

typedef struct voice_s {
    const uint8_t *data;           // Current position in source data (PSRAM)
    const uint8_t *data_end;       // End of sample data
    const uint8_t *loop_start;     // Loop start point (NULL if not looping)
    const uint8_t *loop_end;       // Loop end point

    // Local buffer for mixing (decompressed/converted samples)
    int8_t buffer[VOICE_BUFFER_SAMPLES];
    uint16_t buffer_size;          // Number of valid samples in buffer

    uint32_t offset;               // Current position in buffer (16.16 fixed point)
    uint32_t step;                 // Fixed-point step per output sample (16.16)

    uint8_t left_vol;              // Left channel volume (0-255)
    uint8_t right_vol;             // Right channel volume (0-255)
    uint8_t priority;              // Voice priority for allocation

    bool active;                   // Is this voice playing?
    bool looping;                  // Is this voice looping?
    bool is_16bit;                 // Is sample 16-bit? (false = 8-bit)
    bool is_signed;                // Is sample signed?
    bool is_adpcm;                 // Is sample ADPCM compressed?

    // Creative ADPCM decoder state
    uint8_t adpcm_pred;            // ADPCM predictor (0-255, unsigned)
    int adpcm_step;                // ADPCM step (0-3)

    uint32_t callback_val;         // Value to pass to callback

#if SOUND_LOW_PASS
    uint8_t alpha256;              // Low-pass filter coefficient
#endif
} voice_t;

//=============================================================================
// I2S Driver and Ring Buffer
//=============================================================================

static i2s_config_t i2s_cfg;

// Ring buffer: game loop fills ahead, DMA IRQ consumes via fill callback.
// 3 slots gives ~100 ms of slack for jittery game tics at 30 FPS.
#define MIX_RING_COUNT 3
static int16_t mix_ring[MIX_RING_COUNT][PICO_SOUND_BUFFER_SAMPLES * 2];
static volatile uint32_t mix_rd = 0;  // read by IRQ
static volatile uint32_t mix_wr = 0;  // written by game loop

static audio_buffer_t mix_buffer;

static void setup_mix_buffer_for_slot(uint32_t slot_idx) {
    mix_buffer.buf_storage.bytes = (uint8_t *)mix_ring[slot_idx];
    mix_buffer.buf_storage.size = PICO_SOUND_BUFFER_SAMPLES * 4;
    mix_buffer.buffer = &mix_buffer.buf_storage;
    mix_buffer.max_sample_count = PICO_SOUND_BUFFER_SAMPLES;
    mix_buffer.sample_count = 0;
}

// Called from DMA IRQ -- copies pre-mixed audio to DMA buffer
static void i2s_fill_audio(int buf_index, uint32_t *buf, uint32_t frames) {
    if (mix_rd < mix_wr) {
        uint32_t idx = mix_rd % MIX_RING_COUNT;
        memcpy(buf, mix_ring[idx], frames * sizeof(uint32_t));
        __dmb();
        mix_rd++;
    } else {
        // Underrun -- output silence
        memset(buf, 0, frames * sizeof(uint32_t));
    }
}

//=============================================================================
// Static Variables
//=============================================================================

static bool sound_initialized = false;
static voice_t voices[NUM_SOUND_CHANNELS];
static int next_handle = 1;

static int master_volume = 255;
static bool reverse_stereo = false;
static void (*sound_callback)(int32_t) = NULL;
static void (*music_generator)(audio_buffer_t *buffer) = NULL;

// Deferred callback queue to avoid calling game code from mixer
#define MAX_PENDING_CALLBACKS 32
static volatile uint32_t pending_callbacks[MAX_PENDING_CALLBACKS];
static volatile int pending_callback_head = 0;
static volatile int pending_callback_tail = 0;
static volatile bool processing_callbacks = false;

//=============================================================================
// Creative ADPCM Decoder (VOC codec 4 = Creative 4-bit ADPCM)
// Using DOSBox's table-based algorithm which is known to work correctly
//=============================================================================

// DOSBox's ScaleMap for 4-bit ADPCM (64 entries = 4 steps x 16 nibble values)
static const int8_t adpcm4_scale_map[64] = {
    0,  1,  2,  3,  4,  5,  6,  7,  0,  -1,  -2,  -3,  -4,  -5,  -6,  -7,
    1,  3,  5,  7,  9, 11, 13, 15, -1,  -3,  -5,  -7,  -9, -11, -13, -15,
    2,  6, 10, 14, 18, 22, 26, 30, -2,  -6, -10, -14, -18, -22, -26, -30,
    4, 12, 20, 28, 36, 44, 52, 60, -4, -12, -20, -28, -36, -44, -52, -60
};

// DOSBox's AdjustMap for 4-bit ADPCM - use signed for proper math
static const int8_t adpcm4_adjust_map[64] = {
      0,  0,  0,  0,  0, 16, 16, 16,
      0,  0,  0,  0,  0, 16, 16, 16,
    -16,  0,  0,  0,  0, 16, 16, 16,
    -16,  0,  0,  0,  0, 16, 16, 16,
    -16,  0,  0,  0,  0, 16, 16, 16,
    -16,  0,  0,  0,  0, 16, 16, 16,
    -16,  0,  0,  0,  0,  0,  0,  0,
    -16,  0,  0,  0,  0,  0,  0,  0
};

// Decode one nibble using DOSBox's table-based algorithm
static uint8_t decode_creative_adpcm_nibble(int nibble, uint8_t *reference, int *stepsize) {
    int i = nibble + *stepsize;
    if (i < 0) i = 0;
    if (i > 63) i = 63;

    *stepsize = *stepsize + adpcm4_adjust_map[i];
    if (*stepsize < 0) *stepsize = 0;
    if (*stepsize > 48) *stepsize = 48;

    int new_ref = (int)*reference + adpcm4_scale_map[i];
    if (new_ref < 0) new_ref = 0;
    if (new_ref > 255) new_ref = 255;
    *reference = (uint8_t)new_ref;

    return *reference;
}

//=============================================================================
// Utility Functions
//=============================================================================

static inline int16_t clamp_s16(int32_t v) {
    if (v > INT16_MAX) return INT16_MAX;
    if (v < INT16_MIN) return INT16_MIN;
    return (int16_t)v;
}

static inline uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

// Find a free voice slot or steal one based on priority
static int find_voice_slot(int priority) {
    for (int i = 0; i < NUM_SOUND_CHANNELS; i++) {
        if (!voices[i].active) {
            return i;
        }
    }

    int lowest_priority = priority;
    int lowest_slot = -1;

    for (int i = 0; i < NUM_SOUND_CHANNELS; i++) {
        if (voices[i].priority < lowest_priority) {
            lowest_priority = voices[i].priority;
            lowest_slot = i;
        }
    }

    return lowest_slot;
}

// Convert handle to voice index
static int handle_to_voice(int handle) {
    if (handle <= 0) return -1;
    int voice_idx = (handle - 1) % NUM_SOUND_CHANNELS;
    if (!voices[voice_idx].active) return -1;
    return voice_idx;
}

// Queue a callback to be called later (from game thread, not mixer)
static void queue_callback(uint32_t callback_val) {
    int next_tail = (pending_callback_tail + 1) % MAX_PENDING_CALLBACKS;
    if (next_tail != pending_callback_head) {
        pending_callbacks[pending_callback_tail] = callback_val;
        pending_callback_tail = next_tail;
    }
}

// Process any pending callbacks (called from I_PicoSound_Update)
static void process_pending_callbacks(void) {
    if (processing_callbacks) {
        return;
    }
    processing_callbacks = true;

    int processed = 0;
    while (pending_callback_head != pending_callback_tail && processed < 8) {
        uint32_t cb_val = pending_callbacks[pending_callback_head];
        pending_callback_head = (pending_callback_head + 1) % MAX_PENDING_CALLBACKS;
        processed++;

        if (sound_callback) {
            sound_callback(cb_val);
        }
    }

    processing_callbacks = false;
}

// Decompress/copy next block of samples into voice buffer
static void decompress_buffer(voice_t *v) {
    if (!v || !v->data || !v->data_end || v->data_end < v->data) {
        if (v) v->buffer_size = 0;
        return;
    }

    if (v->data >= v->data_end) {
        if (v->looping && v->loop_start) {
            v->data = v->loop_start;
            if (v->is_adpcm) {
                v->adpcm_pred = 128;
                v->adpcm_step = -1;
            }
        } else {
            v->buffer_size = 0;
            return;
        }
    }

    int samples_decoded = 0;

    if (v->is_adpcm) {
        if (v->adpcm_step < 0 && v->data < v->data_end) {
            v->adpcm_pred = *v->data++;
            v->adpcm_step = 0;
        }

        while (samples_decoded < VOICE_BUFFER_SAMPLES && v->data < v->data_end) {
            uint8_t byte = *v->data++;

            int nibble = (byte >> 4) & 0x0F;
            uint8_t sample = decode_creative_adpcm_nibble(nibble, &v->adpcm_pred, &v->adpcm_step);
            v->buffer[samples_decoded++] = (int8_t)(sample - 128);

            if (samples_decoded >= VOICE_BUFFER_SAMPLES) break;

            nibble = byte & 0x0F;
            sample = decode_creative_adpcm_nibble(nibble, &v->adpcm_pred, &v->adpcm_step);
            v->buffer[samples_decoded++] = (int8_t)(sample - 128);
        }
    } else {
        int available;
        if (v->is_16bit) {
            available = (v->data_end - v->data) / 2;
        } else {
            available = v->data_end - v->data;
        }

        int to_copy = available;
        if (to_copy > VOICE_BUFFER_SAMPLES) {
            to_copy = VOICE_BUFFER_SAMPLES;
        }

        if (to_copy <= 0) {
            v->buffer_size = 0;
            return;
        }

        if (v->is_16bit) {
            const int16_t *src = (const int16_t *)v->data;
            for (int i = 0; i < to_copy; i++) {
                v->buffer[i] = src[i] >> 8;
            }
            v->data += to_copy * 2;
        } else if (v->is_signed) {
            const int8_t *src = (const int8_t *)v->data;
            for (int i = 0; i < to_copy; i++) {
                v->buffer[i] = src[i];
            }
            v->data += to_copy;
        } else {
            const uint8_t *src = v->data;
            for (int i = 0; i < to_copy; i++) {
                v->buffer[i] = (int8_t)(src[i] - 128);
            }
            v->data += to_copy;
        }

        samples_decoded = to_copy;
    }

    v->buffer_size = samples_decoded;
}

// Stop a voice and optionally queue callback
static void stop_voice(int voice_idx, bool do_callback) {
    if (voice_idx < 0 || voice_idx >= NUM_SOUND_CHANNELS) return;

    voice_t *v = &voices[voice_idx];
    uint32_t cb_val = v->callback_val;
    bool was_active = v->active;

    v->active = false;

    if (was_active && do_callback && cb_val != 0) {
        queue_callback(cb_val);
    }
}

//=============================================================================
// VOC/WAV Parsing
//=============================================================================

static bool parse_voc(const uint8_t *data, uint32_t length,
                      const uint8_t **sample_data, uint32_t *sample_length,
                      uint32_t *sample_rate, bool *is_16bit, uint8_t *out_codec) {
    if (length < 26) return false;
    if (memcmp(data, "Creative Voice File\x1a", 20) != 0) return false;

    uint16_t header_size = read_le16(data + 20);
    if (header_size > length) return false;

    const uint8_t *block = data + header_size;
    const uint8_t *end = data + length;

    *is_16bit = false;
    *out_codec = 0;

    while (block < end) {
        uint8_t block_type = block[0];

        if (block_type == 0) break;
        if (block + 4 > end) break;

        uint32_t block_size = block[1] | (block[2] << 8) | (block[3] << 16);
        const uint8_t *block_data = block + 4;

        if (block_data + block_size > end) break;

        switch (block_type) {
            case 1:
                if (block_size < 2) break;
                {
                    uint8_t freq_div = block_data[0];
                    uint8_t codec = block_data[1];

                    if (codec != 0 && codec != 4) {
                        printf("VOC: Unsupported codec %d\n", codec);
                        break;
                    }

                    *out_codec = codec;
                    *sample_rate = 1000000 / (256 - freq_div);
                    *sample_data = block_data + 2;
                    *sample_length = block_size - 2;
                    return true;
                }

            case 9:
                if (block_size < 12) break;
                {
                    *sample_rate = read_le32(block_data);
                    uint8_t bits = block_data[4];
                    uint8_t channels = block_data[5];
                    uint16_t codec = read_le16(block_data + 6);

                    if (codec != 0 && codec != 4) {
                        printf("VOC: Unsupported codec %d\n", codec);
                        break;
                    }
                    if (channels != 1) {
                        printf("VOC: Multi-channel not supported\n");
                        break;
                    }

                    *out_codec = 0;
                    *is_16bit = (bits == 16) || (codec == 4);
                    *sample_data = block_data + 12;
                    *sample_length = block_size - 12;
                    return true;
                }
        }

        block = block_data + block_size;
    }

    return false;
}

static bool parse_wav(const uint8_t *data, uint32_t length,
                      const uint8_t **sample_data, uint32_t *sample_length,
                      uint32_t *sample_rate, bool *is_16bit, bool *is_signed) {
    if (length < 44) return false;
    if (memcmp(data, "RIFF", 4) != 0) return false;
    if (memcmp(data + 8, "WAVE", 4) != 0) return false;

    const uint8_t *ptr = data + 12;
    const uint8_t *end = data + length;

    uint32_t fmt_sample_rate = 0;
    uint16_t bits_per_sample = 0;
    uint16_t audio_format = 0;
    bool found_fmt = false;

    while (ptr + 8 <= end) {
        uint32_t chunk_id = read_le32(ptr);
        uint32_t chunk_size = read_le32(ptr + 4);
        const uint8_t *chunk_data = ptr + 8;

        if (chunk_data + chunk_size > end) break;

        if (chunk_id == 0x20746D66) {  // "fmt "
            if (chunk_size < 16) return false;

            audio_format = read_le16(chunk_data);
            uint16_t channels = read_le16(chunk_data + 2);
            fmt_sample_rate = read_le32(chunk_data + 4);
            bits_per_sample = read_le16(chunk_data + 14);

            if (audio_format != 1) {
                printf("WAV: Only PCM format supported\n");
                return false;
            }
            if (channels != 1) {
                printf("WAV: Only mono supported, got %d channels\n", channels);
            }

            found_fmt = true;
        }
        else if (chunk_id == 0x61746164) {  // "data"
            if (!found_fmt) return false;

            *sample_data = chunk_data;
            *sample_length = chunk_size;
            *sample_rate = fmt_sample_rate;
            *is_16bit = (bits_per_sample == 16);
            *is_signed = (bits_per_sample == 16);
            return true;
        }

        ptr = chunk_data + chunk_size;
        if (chunk_size & 1) ptr++;
    }

    return false;
}

//=============================================================================
// Audio Mixing
//=============================================================================

static void mix_audio_buffer(audio_buffer_t *buffer) {
    int16_t *samples = (int16_t *)buffer->buffer->bytes;
    int sample_count = buffer->max_sample_count;

    // Start with silence or music
    if (music_generator) {
        music_generator(buffer);
    } else {
        memset(samples, 0, sample_count * 4);  // 2 channels * 2 bytes
    }

    // Mix in all active voices
    for (int ch = 0; ch < NUM_SOUND_CHANNELS; ch++) {
        voice_t *v = &voices[ch];
        if (!v->active) continue;
        if (v->buffer_size == 0) continue;

        int voll = v->left_vol / 4;
        int volr = v->right_vol / 4;

        if (reverse_stereo) {
            int tmp = voll;
            voll = volr;
            volr = tmp;
        }

        uint32_t offset_end = v->buffer_size * 65536;
        int16_t *out = samples;

        if ((v->offset >> 16) >= VOICE_BUFFER_SAMPLES) {
            v->offset = 0;
        }

#if SOUND_LOW_PASS
        int alpha256 = v->alpha256;
        int beta256 = 256 - alpha256;
        int sample = v->buffer[v->offset >> 16];
#endif

        int decompress_calls = 0;

        for (int s = 0; s < sample_count; s++) {
            uint32_t buf_idx = v->offset >> 16;
            if (buf_idx >= VOICE_BUFFER_SAMPLES) {
                v->active = false;
                break;
            }

#if !SOUND_LOW_PASS
            int sample = v->buffer[buf_idx];
#else
            sample = (beta256 * sample + alpha256 * v->buffer[buf_idx]) / 256;
#endif

            int32_t mixed_0 = out[0];
            int32_t mixed_1 = out[1];

            mixed_0 += sample * voll;
            mixed_1 += sample * volr;

            out[0] = clamp_s16(mixed_0);
            out[1] = clamp_s16(mixed_1);

            out += 2;
            v->offset += v->step;

            if (v->offset >= offset_end) {
                v->offset -= offset_end;

                decompress_calls++;
                if (decompress_calls > 20) {
                    v->active = false;
                    break;
                }

                decompress_buffer(v);

                offset_end = v->buffer_size * 65536;
                if (offset_end == 0) {
                    if (v->callback_val != 0) {
                        queue_callback(v->callback_val);
                    }
                    v->active = false;
                    break;
                }
                if (v->offset >= offset_end) {
                    v->offset = 0;
                }
            }
        }
    }

    buffer->sample_count = sample_count;
}

//=============================================================================
// Public Interface
//=============================================================================

bool I_PicoSound_Init(int numvoices, int mixrate) {
    if (sound_initialized) return true;

    i2s_cfg = (i2s_config_t){
        .sample_freq = PICO_SOUND_SAMPLE_FREQ,
        .channel_count = 2,
        .data_pin = I2S_DATA_PIN,
        .clock_pin_base = I2S_CLOCK_PIN_BASE,
        .pio = pio0,
        .dma_trans_count = PICO_SOUND_BUFFER_SAMPLES,
        .volume = 0,  // max volume (no attenuation)
    };

    printf("I_PicoSound_Init: I2S init (PIO0 pins D%d CLK%d, %d Hz, %d samples/buf, ring=%d)\n",
           I2S_DATA_PIN, I2S_CLOCK_PIN_BASE,
           PICO_SOUND_SAMPLE_FREQ, PICO_SOUND_BUFFER_SAMPLES, MIX_RING_COUNT);

    i2s_init(&i2s_cfg);

    // Pre-fill ring buffer with silence, then register fill callback
    memset(mix_ring, 0, sizeof(mix_ring));
    mix_rd = 0;
    mix_wr = 0;
    i2s_set_fill_callback(i2s_fill_audio);
    i2s_start();

    // Initialize voices
    memset(voices, 0, sizeof(voices));

    sound_initialized = true;
    printf("I_PicoSound_Init: initialization complete\n");
    return true;
}

void I_PicoSound_Shutdown(void) {
    if (!sound_initialized) return;

    i2s_deinit(&i2s_cfg);
    sound_initialized = false;
}

void I_PicoSound_Update(void) {
    if (!sound_initialized) return;

    // Pre-fill ring buffer as far ahead as possible
    while ((mix_wr - mix_rd) < MIX_RING_COUNT) {
        uint32_t idx = mix_wr % MIX_RING_COUNT;
        setup_mix_buffer_for_slot(idx);
        mix_audio_buffer(&mix_buffer);
        __dmb();
        mix_wr++;
    }

    // Process any pending callbacks from finished sounds
    process_pending_callbacks();
}

bool I_PicoSound_IsInitialized(void) {
    return sound_initialized;
}

int I_PicoSound_PlayVOC(const uint8_t *data, uint32_t length,
                        int samplerate, int pitchoffset,
                        int vol, int left, int right,
                        int priority, uint32_t callbackval,
                        bool looping, uint32_t loopstart, uint32_t loopend) {
    if (!sound_initialized) return 0;

    const uint8_t *sample_data;
    uint32_t sample_length, sample_rate;
    bool is_16bit;
    uint8_t codec = 0;

    if (!parse_voc(data, length, &sample_data, &sample_length, &sample_rate, &is_16bit, &codec)) {
        sample_data = data;
        sample_length = length;
        sample_rate = samplerate > 0 ? samplerate : 11025;
        is_16bit = false;
        codec = 0;
    }

    bool is_adpcm = (codec == 4);

    int slot = find_voice_slot(priority);
    if (slot < 0) return 0;

    voice_t *v = &voices[slot];
    stop_voice(slot, true);

    v->data = sample_data;
    v->data_end = sample_data + sample_length;

    v->loop_start = looping ? sample_data : NULL;
    v->loop_end = looping ? sample_data + sample_length : NULL;
    v->looping = looping;

    v->is_16bit = is_16bit;
    v->is_signed = false;
    v->is_adpcm = is_adpcm;

    if (is_adpcm) {
        v->adpcm_pred = 128;
        v->adpcm_step = -1;
    }

    decompress_buffer(v);
    v->offset = 0;

    int32_t rate = (int32_t)sample_rate;
    if (pitchoffset != 0) {
        rate = rate + (rate * pitchoffset / 2048);
        if (rate < 1000) rate = 1000;
        if (rate > 48000) rate = 48000;
    }
    v->step = ((uint64_t)rate << 16) / PICO_SOUND_SAMPLE_FREQ;

    if (left <= 0 && right <= 0 && vol > 0) {
        left = vol;
        right = vol;
    }
    left = left * 4;
    right = right * 4;
    v->left_vol = left > 255 ? 255 : (left < 0 ? 0 : left);
    v->right_vol = right > 255 ? 255 : (right < 0 ? 0 : right);
    v->priority = priority;
    v->callback_val = callbackval;

#if SOUND_LOW_PASS
    v->alpha256 = (256 * 201 * sample_rate) / (201 * sample_rate + 64 * PICO_SOUND_SAMPLE_FREQ);
#endif

    v->active = true;

    int handle = (next_handle++ % 10000) * NUM_SOUND_CHANNELS + slot + 1;
    return handle;
}

int I_PicoSound_PlayWAV(const uint8_t *data, uint32_t length,
                        int pitchoffset,
                        int vol, int left, int right,
                        int priority, uint32_t callbackval,
                        bool looping, uint32_t loopstart, uint32_t loopend) {
    if (!sound_initialized) return 0;

    const uint8_t *sample_data;
    uint32_t sample_length, sample_rate;
    bool is_16bit, is_signed;

    if (!parse_wav(data, length, &sample_data, &sample_length, &sample_rate, &is_16bit, &is_signed)) {
        printf("I_PicoSound_PlayWAV: Failed to parse WAV\n");
        return 0;
    }

    int slot = find_voice_slot(priority);
    if (slot < 0) return 0;

    voice_t *v = &voices[slot];
    stop_voice(slot, true);

    v->data = sample_data;
    v->data_end = sample_data + sample_length;

    v->loop_start = looping ? sample_data : NULL;
    v->loop_end = looping ? sample_data + sample_length : NULL;
    v->looping = looping;

    v->is_16bit = is_16bit;
    v->is_signed = is_signed;
    v->is_adpcm = false;

    decompress_buffer(v);
    v->offset = 0;

    int32_t rate = (int32_t)sample_rate;
    if (pitchoffset != 0) {
        rate = rate + (rate * pitchoffset / 2048);
        if (rate < 1000) rate = 1000;
        if (rate > 48000) rate = 48000;
    }
    v->step = ((uint64_t)rate << 16) / PICO_SOUND_SAMPLE_FREQ;

    if (left <= 0 && right <= 0 && vol > 0) {
        left = vol;
        right = vol;
    }
    left = left * 4;
    right = right * 4;
    v->left_vol = left > 255 ? 255 : (left < 0 ? 0 : left);
    v->right_vol = right > 255 ? 255 : (right < 0 ? 0 : right);
    v->priority = priority;
    v->callback_val = callbackval;

#if SOUND_LOW_PASS
    v->alpha256 = (256 * 201 * sample_rate) / (201 * sample_rate + 64 * PICO_SOUND_SAMPLE_FREQ);
#endif

    v->active = true;

    int handle = (next_handle++ % 10000) * NUM_SOUND_CHANNELS + slot + 1;
    return handle;
}

int I_PicoSound_PlayRaw(const uint8_t *data, uint32_t length,
                        uint32_t samplerate, int pitchoffset,
                        int vol, int left, int right,
                        int priority, uint32_t callbackval,
                        bool looping, const uint8_t *loopstart, const uint8_t *loopend) {
    if (!sound_initialized) return 0;
    if (!data || length == 0) return 0;

    int slot = find_voice_slot(priority);
    if (slot < 0) return 0;

    voice_t *v = &voices[slot];
    stop_voice(slot, true);

    v->data = data;
    v->data_end = data + length;
    v->loop_start = loopstart;
    v->loop_end = loopend;
    v->looping = looping;

    v->is_16bit = false;
    v->is_signed = false;
    v->is_adpcm = false;

    decompress_buffer(v);
    v->offset = 0;

    int32_t rate = (int32_t)samplerate;
    if (pitchoffset != 0) {
        rate = rate + (rate * pitchoffset / 2048);
        if (rate < 1000) rate = 1000;
        if (rate > 48000) rate = 48000;
    }
    v->step = ((uint64_t)rate << 16) / PICO_SOUND_SAMPLE_FREQ;

    if (left <= 0 && right <= 0 && vol > 0) {
        left = vol;
        right = vol;
    }
    left = left * 4;
    right = right * 4;
    v->left_vol = left > 255 ? 255 : (left < 0 ? 0 : left);
    v->right_vol = right > 255 ? 255 : (right < 0 ? 0 : right);
    v->priority = priority;
    v->callback_val = callbackval;

#if SOUND_LOW_PASS
    v->alpha256 = (256 * 201 * samplerate) / (201 * samplerate + 64 * PICO_SOUND_SAMPLE_FREQ);
#endif

    v->active = true;

    int handle = (next_handle++ % 10000) * NUM_SOUND_CHANNELS + slot + 1;
    return handle;
}

int I_PicoSound_StopVoice(int handle) {
    int slot = handle_to_voice(handle);
    if (slot >= 0) {
        stop_voice(slot, false);
        return 1;
    }
    return 0;
}

void I_PicoSound_StopAllVoices(void) {
    for (int i = 0; i < NUM_SOUND_CHANNELS; i++) {
        stop_voice(i, false);
    }
}

bool I_PicoSound_VoicePlaying(int handle) {
    int slot = handle_to_voice(handle);
    return slot >= 0 && voices[slot].active;
}

int I_PicoSound_VoicesPlaying(void) {
    int count = 0;
    for (int i = 0; i < NUM_SOUND_CHANNELS; i++) {
        if (voices[i].active) count++;
    }
    return count;
}

bool I_PicoSound_VoiceAvailable(int priority) {
    return find_voice_slot(priority) >= 0;
}

void I_PicoSound_SetPan(int handle, int vol, int left, int right) {
    int slot = handle_to_voice(handle);
    if (slot < 0) return;

    voices[slot].left_vol = left > 255 ? 255 : (left < 0 ? 0 : left);
    voices[slot].right_vol = right > 255 ? 255 : (right < 0 ? 0 : right);
}

void I_PicoSound_SetPitch(int handle, int pitchoffset) {
    // Not implemented
}

void I_PicoSound_SetFrequency(int handle, int frequency) {
    int slot = handle_to_voice(handle);
    if (slot < 0) return;

    voices[slot].step = ((uint32_t)frequency << 16) / PICO_SOUND_SAMPLE_FREQ;
}

void I_PicoSound_EndLooping(int handle) {
    int slot = handle_to_voice(handle);
    if (slot < 0) return;

    voices[slot].looping = false;
    voices[slot].loop_start = NULL;
}

void I_PicoSound_Pan3D(int handle, int angle, int distance) {
    int slot = handle_to_voice(handle);
    if (slot < 0) return;

    int vol = 255 - distance;
    if (vol < 0) vol = 0;

    int pan;
    if (angle < 128) {
        pan = angle * 2;
    } else {
        pan = (256 - angle) * 2;
    }

    voices[slot].left_vol = (vol * (255 - pan)) >> 8;
    voices[slot].right_vol = (vol * pan) >> 8;
}

void I_PicoSound_SetVolume(int volume) {
    master_volume = volume > 255 ? 255 : (volume < 0 ? 0 : volume);
}

int I_PicoSound_GetVolume(void) {
    return master_volume;
}

void I_PicoSound_SetReverseStereo(bool reverse) {
    reverse_stereo = reverse;
}

bool I_PicoSound_GetReverseStereo(void) {
    return reverse_stereo;
}

void I_PicoSound_SetCallback(void (*callback)(int32_t)) {
    sound_callback = callback;
}

void I_PicoSound_SetMusicGenerator(void (*generator)(audio_buffer_t *buffer)) {
    music_generator = generator;
}
