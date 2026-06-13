#pragma once
#include <stdint.h>
#include <stddef.h>

// Audio HAL — speaker + microphone through an on-board codec.
//
// Boards without audio hardware ship a stub that returns false from
// audio_hal_init(); shared code (voice.cpp) checks board_caps().has_audio
// and the init result before starting any audio work.
//
// Sample format at this interface: 16 kHz, mono, signed 16-bit PCM.
// Stereo/mono conversion for the codec happens inside the board impl.

// Bring up codec + I2S. Returns true if audio is usable. Safe to call once
// from setup() after board_init() (I2C must already be up).
bool audio_hal_init(void);

// Blocking playback write. Returns samples accepted.
size_t audio_hal_write(const int16_t* mono, size_t samples);

// Block until queued playback has left the DMA buffers.
void audio_hal_drain(void);

// Blocking mic read of exactly `samples` mono samples (or fewer on timeout).
size_t audio_hal_read(int16_t* mono, size_t samples);

// Power amp / codec output enable (saves power + avoids hiss when idle).
void audio_hal_set_amp(bool on);
