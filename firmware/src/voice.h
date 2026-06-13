#pragma once
#include <stdint.h>

// BLE voice channel + approval overlay.
//
// Counterpart of the Mac daemon's voice_approver/device.py — protocol doc
// lives there. Service ...0010 with four characteristics:
//   AUDIO_OUT ...0011  write-no-response  [seq:u8][flags:u8][adpcm], flags&1 = last
//   CTRL      ...0012  write              JSON: ask / rec / stop / idle
//   AUDIO_IN  ...0013  notify             same framing as AUDIO_OUT
//   EVT       ...0014  notify             JSON: play_done / rec_done / touch
// Audio both ways: IMA ADPCM (no block headers), 16 kHz mono PCM16.
//
// The overlay (ask text + Allow/Deny) works on every board; speaker/mic
// only where board_caps().has_audio and audio_hal_init() succeed.

class NimBLEServer;

// Create the GATT service. Call from ble_init() BEFORE server->start().
void voice_register_ble(NimBLEServer* server);

// Bring up audio + overlay state. Call from setup() after ui_init().
void voice_init(void);

// Pump events: overlay show/hide, EVT notifies, audio-in frames.
// Call every loop() iteration (LVGL work happens only here).
void voice_tick(void);

// True while the approval overlay is visible (lets other UI yield).
bool voice_overlay_active(void);
