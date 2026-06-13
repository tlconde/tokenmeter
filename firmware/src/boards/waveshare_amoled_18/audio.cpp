// Audio HAL for the Waveshare ESP32-S3-Touch-AMOLED-1.8.
//
// ES8311 codec @ I2C 0x18 (shared bus, SDA=15 SCL=14, brought up by
// board_init), I2S full duplex via the Arduino core's ESP_I2S wrapper.
// Pin numbers come from Waveshare's BSP (esp32_s3_touch_amoled_1_8.h) and
// the 15_ES8311 Arduino demo in the vendor repo:
//   BCLK=9  MCLK=16  LRCK/WS=45  DOUT=8 (codec DAC)  DIN=10 (codec ADC)
//   Power amp enable: GPIO46 (and EXIO2 on the XCA9554 gates the amp too).
//
// Interface format is 16 kHz mono PCM16; the I2S link runs stereo 16-bit
// (proven config from the vendor demo) so we duplicate/select samples here.

#include <Arduino.h>
#include "ESP_I2S.h"
#include "../../hal/audio_hal.h"
#include "board.h"
#include "board_i2c.h"
#include "io_expander.h"
#include "es8311.h"

#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_VOLUME      85     // 0..100 speaker volume
#define AUDIO_MIC_GAIN    ES8311_MIC_GAIN_18DB

#define PIN_I2S_BCK   9
#define PIN_I2S_MCLK  16
#define PIN_I2S_WS    45
#define PIN_I2S_DOUT  8    // ESP -> codec (speaker)
#define PIN_I2S_DIN   10   // codec -> ESP (mic)
#define PIN_PA_EN     46

static I2SClass i2s;
static bool audio_ok = false;

// Scratch buffer for mono<->stereo conversion, sized for voice.cpp's largest
// burst (360 samples = one 180-byte ADPCM chunk = 22.5 ms).
#define CONV_MAX_SAMPLES 512
static int16_t conv_buf[CONV_MAX_SAMPLES * 2];

extern "C" esp_err_t waveshare_i2c_write(uint8_t addr, const uint8_t* data,
                                          size_t len) {
    BoardWire.beginTransmission(addr);
    if (BoardWire.write(data, len) != len) {
        BoardWire.endTransmission();
        return ESP_FAIL;
    }
    return BoardWire.endTransmission() == 0 ? ESP_OK : ESP_FAIL;
}

extern "C" esp_err_t waveshare_i2c_write_read(uint8_t addr,
                                               const uint8_t* write_data,
                                               size_t write_len,
                                               uint8_t* read_data,
                                               size_t read_len) {
    BoardWire.beginTransmission(addr);
    if (BoardWire.write(write_data, write_len) != write_len ||
        BoardWire.endTransmission(false) != 0) {
        return ESP_FAIL;
    }
    if (BoardWire.requestFrom(addr, (uint8_t)read_len) != read_len) {
        return ESP_FAIL;
    }
    for (size_t i = 0; i < read_len; ++i) {
        read_data[i] = BoardWire.read();
    }
    return ESP_OK;
}

bool audio_hal_init(void) {
    // Amp enable: direct GPIO per the vendor demo, plus the IO expander
    // line the schematic routes to the amp as well. Start muted.
    pinMode(PIN_PA_EN, OUTPUT);
    digitalWrite(PIN_PA_EN, LOW);
    io_expander_set(IOX_PIN_PA_EN, false);

    i2s.setPins(PIN_I2S_BCK, PIN_I2S_WS, PIN_I2S_DOUT, PIN_I2S_DIN, PIN_I2S_MCLK);
    if (!i2s.begin(I2S_MODE_STD, AUDIO_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                   I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
        Serial.println("audio: I2S init failed");
        return false;
    }

    // BoardWire (I2C port 1) is already up via board_init().
    es8311_handle_t es = es8311_create(1, ES8311_ADDRESS_0);
    if (!es) {
        Serial.println("audio: ES8311 create failed");
        return false;
    }
    const es8311_clock_config_t clk = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = AUDIO_SAMPLE_RATE * 256,
        .sample_frequency = AUDIO_SAMPLE_RATE,
    };
    if (es8311_init(es, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) {
        Serial.println("audio: ES8311 init failed");
        return false;
    }
    es8311_sample_frequency_config(es, clk.mclk_frequency, clk.sample_frequency);
    es8311_microphone_config(es, false);
    es8311_voice_volume_set(es, AUDIO_VOLUME, NULL);
    es8311_microphone_gain_set(es, AUDIO_MIC_GAIN);

    audio_ok = true;
    Serial.println("audio: ES8311 ready (16 kHz duplex)");
    return true;
}

void audio_hal_set_amp(bool on) {
    if (!audio_ok) return;
    digitalWrite(PIN_PA_EN, on ? HIGH : LOW);
    io_expander_set(IOX_PIN_PA_EN, on);
}

size_t audio_hal_write(const int16_t* mono, size_t samples) {
    if (!audio_ok) return 0;
    size_t done = 0;
    while (done < samples) {
        size_t n = samples - done;
        if (n > CONV_MAX_SAMPLES) n = CONV_MAX_SAMPLES;
        for (size_t i = 0; i < n; i++) {
            conv_buf[2 * i]     = mono[done + i];
            conv_buf[2 * i + 1] = mono[done + i];
        }
        size_t wrote = i2s.write((uint8_t*)conv_buf, n * 4);
        if (wrote == 0) break;
        done += wrote / 4;
    }
    return done;
}

void audio_hal_drain(void) {
    if (!audio_ok) return;
    // ESP_I2S has no explicit drain; DMA depth is small (~6 x 240 frames
    // default ≈ 90 ms at 16 kHz). A short wait empties it.
    delay(120);
}

size_t audio_hal_read(int16_t* mono, size_t samples) {
    if (!audio_ok) return 0;
    size_t done = 0;
    while (done < samples) {
        size_t n = samples - done;
        if (n > CONV_MAX_SAMPLES) n = CONV_MAX_SAMPLES;
        size_t got = i2s.readBytes((char*)conv_buf, n * 4);
        if (got == 0) break;
        size_t frames = got / 4;
        for (size_t i = 0; i < frames; i++) {
            mono[done + i] = conv_buf[2 * i];  // left slot carries the mic
        }
        done += frames;
    }
    return done;
}
