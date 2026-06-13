// Audio HAL stub — no codec wired up on this port yet. The AMOLED-2.16
// does carry an ES8311; porting the waveshare_amoled_18 audio.cpp is the
// path if device audio is ever wanted here.
#include "../../hal/audio_hal.h"

bool audio_hal_init(void) { return false; }
size_t audio_hal_write(const int16_t*, size_t) { return 0; }
void audio_hal_drain(void) {}
size_t audio_hal_read(int16_t*, size_t) { return 0; }
void audio_hal_set_amp(bool) {}
